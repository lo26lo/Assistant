#pragma once

#include <QObject>
#include <QString>
#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "camera/CameraCapture.h"  // ibom::camera::FrameRef
#include "ibom/IBomData.h"

namespace ibom::features {

// ============================================================================
// Class mapping: footprint/reference/value → detector class id
// ============================================================================

/**
 * @brief Maps a Component to a YOLO class id using ordered regex rules
 *        loaded from footprint_classes.json.
 *
 * Rules are evaluated in order; the first rule whose present fields ALL
 * match wins. Components matching no rule fall into "other" and their
 * footprint is logged once (never silently classified).
 */
class ClassMapper {
public:
    /// Load classes + rules from a JSON file. Returns false on parse error
    /// or if the mandatory "classes"/"rules" keys are missing.
    bool load(const std::string& jsonPath);

    /// Same, from an already-parsed JSON document (used by tests).
    bool loadJson(const nlohmann::json& j);

    bool isLoaded() const { return !m_classes.empty(); }

    /// Class id for a component (index into classNames()). Unmatched
    /// components map to the "other" class.
    int classId(const Component& c);

    const std::vector<std::string>& classNames() const { return m_classes; }

    /// Unique footprints that matched no rule so far (for rule curation).
    const std::set<std::string>& unmappedFootprints() const { return m_unmapped; }

private:
    struct Rule {
        int classIdx = -1;
        // Empty pattern = field not constrained. Case-insensitive search.
        std::string footprint, ref, value;
    };

    std::vector<std::string> m_classes;
    std::vector<Rule>        m_rules;
    int                      m_otherIdx = -1;
    std::set<std::string>    m_unmapped;
};

// ============================================================================
// Label projection (pure function — unit-tested without Qt event loop)
// ============================================================================

struct LabelParams {
    double shrink         = 0.85;  ///< bbox shrink factor (iBOM bbox ≥ visual extent)
    int    minBoxPx       = 12;    ///< reject boxes smaller than this (either dim)
    double minVisibleFrac = 0.6;   ///< reject components clipped more than this
};

struct YoloLabel {
    int    classId = 0;
    double cx = 0, cy = 0, w = 0, h = 0;  ///< normalized [0,1], YOLO convention
};

/// Project components of `layer` into the image through homography `H`
/// (PCB → image, CV_64F 3×3) and produce normalized YOLO labels.
/// Applies shrink, image clipping, visibility and minimum-size gates.
std::vector<YoloLabel> projectLabels(const std::vector<Component>& components,
                                     Layer layer,
                                     const cv::Mat& H,
                                     cv::Size imageSize,
                                     const LabelParams& params,
                                     ClassMapper& mapper);

// ============================================================================
// Live status published to the GUI panel
// ============================================================================

struct DatasetStatus {
    // Gates (all must be true for a frame to be eligible)
    bool gateTracking  = false;  ///< inliers ≥ threshold
    bool gateReproj    = false;  ///< median reprojection error ≤ threshold
    bool gateSharpness = false;  ///< Laplacian variance ≥ threshold
    bool gateExposure  = false;  ///< saturated/black pixel fraction ≤ threshold
    bool gateFresh     = false;  ///< homography age ≤ threshold

    // Raw values behind the gates (for display)
    int    inliers         = 0;
    double reprojErrPx     = 0.0;
    double sharpness       = 0.0;
    double badExposureFrac = 0.0;
    double homographyAgeMs = 1e9;

    // Session counters
    int  saved          = 0;
    int  rejectedGates  = 0;   ///< at least one gate red
    int  rejectedPose   = 0;   ///< gates ok but pose too close to last saved
    int  rejectedLabels = 0;   ///< gates ok but no usable label in frame
    int  lastLabelCount = 0;
    bool sessionActive  = false;
};

// ============================================================================
// DatasetCreator — capture worker (lives on its own QThread)
// ============================================================================

/**
 * @brief Auto-annotation capture: every frame where live tracking is locked
 *        and quality gates pass becomes an annotated YOLO training image.
 *
 * Owned by Application, moved to a dedicated QThread (JPEG encoding must not
 * block the GUI). Receives frames via processFrame() and tracking quality
 * via onHomography(), both through queued connections.
 *
 * Output layout under <dataDir>/dataset/session_<date>_<board>_<lighting>/:
 *   images/frame_NNNNNN.jpg   labels/frame_NNNNNN.txt   manifest.jsonl
 * — exactly what PCB Dataset Studio (tools/dataset_studio) imports.
 */
class DatasetCreator : public QObject {
    Q_OBJECT

public:
    explicit DatasetCreator(QObject* parent = nullptr);
    ~DatasetCreator() override = default;

public slots:
    /// Thresholds for the quality gates and save throttling.
    void configure(int minInliers, double maxReprojErrPx, double minSharpness,
                   double maxBadExposureFrac, int maxHomographyAgeMs,
                   int saveIntervalMs, double minPoseDeltaPx,
                   double bboxShrink, int minBoxPx, double minVisibleFrac);

    /// iBOM project + the board face currently aligned with the camera.
    /// Types fully qualified on purpose: the moc signature string must match
    /// the qRegisterMetaType()/Q_ARG strings exactly (CLAUDE.md pitfall #17).
    void setProject(std::shared_ptr<const ibom::IBomProject> project, ibom::Layer layer);

    /// Path of footprint_classes.json (resolved by Application at startup).
    void setClassRulesPath(QString path);

    /// Root directory for sessions (<dataDir>/dataset).
    void setOutputRoot(QString root);

    /// Start a capture session. Emits sessionStarted(dir) or sessionError().
    void startSession(QString boardName, QString lightingTag);

    /// Stop the session; emits sessionStopped(savedCount).
    void stopSession();

    /// Tracking quality feed (connected to TrackingWorker::homographyUpdated).
    void onHomography(cv::Mat h, int inliers, double reprojErrPx);

    /// Camera frame feed (connected to CameraCapture::frameReady).
    void processFrame(ibom::camera::FrameRef frame);

signals:
    void sessionStarted(QString directory);
    void sessionStopped(int savedCount);
    void sessionError(QString message);
    void statusUpdated(ibom::features::DatasetStatus status);

private:
    bool evaluateGates(const cv::Mat& gray, DatasetStatus& st) const;
    bool poseMovedEnough(const cv::Mat& h) const;
    void writeFrame(const cv::Mat& frame, const std::vector<YoloLabel>& labels,
                    const DatasetStatus& st);

    // Configuration (gates / throttle / labels)
    int    m_minInliers         = 25;
    double m_maxReprojErrPx     = 3.0;
    double m_minSharpness       = 100.0;
    double m_maxBadExposureFrac = 0.05;
    int    m_maxHomographyAgeMs = 300;
    int    m_saveIntervalMs     = 500;   ///< ≈ 2 img/s max
    double m_minPoseDeltaPx     = 15.0;
    LabelParams m_labelParams;

    // Inputs
    std::shared_ptr<const IBomProject> m_project;
    Layer       m_layer = Layer::Front;
    QString     m_classRulesPath;
    QString     m_outputRoot;
    ClassMapper m_mapper;

    // Tracking state (only touched from the worker thread)
    cv::Mat m_homography;
    int     m_inliers = 0;
    double  m_reprojErrPx = 0.0;
    std::chrono::steady_clock::time_point m_lastHomographyTime;

    // Session state
    bool                  m_active = false;
    std::filesystem::path m_sessionDir;
    std::ofstream         m_manifest;
    QString               m_boardName, m_lightingTag;
    int                   m_frameIndex = 0;
    cv::Mat               m_lastSavedPose;  ///< homography of last saved frame
    std::chrono::steady_clock::time_point m_lastSaveTime;
    std::chrono::steady_clock::time_point m_lastStatusTime;
    DatasetStatus         m_status;
};

} // namespace ibom::features

Q_DECLARE_METATYPE(ibom::features::DatasetStatus)
