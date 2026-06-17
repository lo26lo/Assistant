#pragma once

#include <QDialog>
#include <QStringList>
#include <QElapsedTimer>
#include <opencv2/core.hpp>

#include "../camera/ICameraSource.h"   // ibom::camera::FrameRef

class QLabel;
class QTextEdit;
class QPushButton;

namespace ibom { class Config; }

namespace ibom::gui {

/**
 * Dev tool — live calibration cockpit.
 *
 * A non-modal pop-up (the user keeps interacting with the main window) that
 * shows, in real time, everything needed to judge whether a checkerboard
 * calibration will succeed BEFORE committing the 5 shots:
 *
 *   • live preview with the detected checkerboard corners drawn,
 *   • detection verdict (found? corner count vs expected, method),
 *   • sharpness (Laplacian variance) vs the project threshold,
 *   • board coverage % and centroid quadrant (to vary the pose per shot),
 *   • mean brightness with over/under-exposure hints,
 *   • the current calibration parameters and on-disk calibration state,
 *   • capture progress (N / 5),
 *   • a rolling feed of WARN/ERROR log lines (calibration ones flagged),
 *   • a one-line readiness verdict at the top.
 *
 * A "Copy report" button assembles all of the above (plus the recent problem
 * log) as plain text on the clipboard, so it can be pasted straight back to a
 * developer/assistant for diagnosis. A "Capture image" button re-emits the
 * normal calibration-capture request so the whole flow can be driven from here.
 *
 * The dialog never touches the camera itself: Application feeds it frames via
 * onFrame() and pushes calibration/progress state. All detection runs on the
 * GUI thread, throttled, on a downscaled copy.
 */
class CalibrationMonitorDialog : public QDialog {
    Q_OBJECT

public:
    explicit CalibrationMonitorDialog(const ibom::Config& config,
                                      QWidget* parent = nullptr);

    /// Feed the latest camera frame (GUI thread). Heavy work is throttled
    /// internally, so it is safe to call at full frame rate.
    void onFrame(const ibom::camera::FrameRef& frame);

    /// Push the active camera / calibration state (called on open, after a
    /// calibration run, and on every backend switch).
    void setCalibrationStatus(bool calibrated, double rmsErr, double pixelsPerMm,
                              const QString& backendName, int camW, int camH,
                              const QString& calibFilePath);

    /// Push calibration-capture progress (images grabbed in the current run).
    void setCaptureProgress(bool collecting, int captured, int total);

signals:
    /// User clicked "Capture image" — wired to the normal calibration request.
    void captureRequested();

public slots:
    /// Receives every spdlog record (via LogBridge). Only WARN+ is kept.
    void appendLog(int level, const QString& logger, const QString& message);

private:
    void buildUi();
    void refreshDetection();          // run detection on m_lastFrame, update UI
    void updateVerdict();             // recompute the top readiness banner
    QString buildReport() const;      // plain-text snapshot for the clipboard

    const ibom::Config& m_config;

    // ── Live frame / detection state ────────────────────────────────
    cv::Mat       m_lastFrame;        // BGR or gray clone of the latest frame
    QElapsedTimer m_throttle;         // limits detection rate
    bool          m_haveFrame   = false;
    bool          m_detected    = false;
    int           m_cornersFound = 0;
    int           m_cornersExpected = 0;
    QString       m_method;           // "sector-based", "legacy", "—"
    double        m_sharpness   = 0.0;
    double        m_brightness  = 0.0;
    double        m_coveragePct = 0.0;
    QString       m_quadrant;         // where the board sits in the frame

    // ── Pushed calibration state ────────────────────────────────────
    bool    m_calibrated     = false;
    double  m_rmsErr         = 0.0;
    double  m_pixelsPerMm    = 0.0;
    int     m_camW           = 0;
    int     m_camH           = 0;
    QString m_backendName;
    QString m_calibFilePath;
    bool    m_collecting     = false;
    int     m_captured       = 0;
    int     m_captureTotal   = 5;

    // ── Rolling problem log ─────────────────────────────────────────
    QStringList m_logLines;           // capped; WARN/ERROR only

    // ── Widgets ─────────────────────────────────────────────────────
    QLabel*    m_verdict      = nullptr;
    QLabel*    m_preview      = nullptr;
    QLabel*    m_lblDetect    = nullptr;
    QLabel*    m_lblSharp     = nullptr;
    QLabel*    m_lblCoverage  = nullptr;
    QLabel*    m_lblBright    = nullptr;
    QLabel*    m_lblParams    = nullptr;
    QLabel*    m_lblCalib     = nullptr;
    QLabel*    m_lblScale     = nullptr;
    QLabel*    m_lblProgress  = nullptr;
    QTextEdit* m_logView      = nullptr;
    QPushButton* m_copyBtn    = nullptr;
};

} // namespace ibom::gui
