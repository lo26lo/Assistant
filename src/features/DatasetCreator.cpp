#include "DatasetCreator.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <QDateTime>
#include <QRegularExpression>

#include <algorithm>
#include <regex>

namespace ibom::features {

namespace fs = std::filesystem;

// ============================================================================
// ClassMapper
// ============================================================================

bool ClassMapper::load(const std::string& jsonPath)
{
    try {
        std::ifstream ifs(jsonPath);
        if (!ifs) {
            spdlog::error("ClassMapper: cannot open '{}'", jsonPath);
            return false;
        }
        return loadJson(nlohmann::json::parse(ifs));
    } catch (const std::exception& e) {
        spdlog::error("ClassMapper: failed to parse '{}': {}", jsonPath, e.what());
        return false;
    }
}

bool ClassMapper::loadJson(const nlohmann::json& j)
{
    if (!j.contains("classes") || !j.contains("rules")) {
        spdlog::error("ClassMapper: missing 'classes' or 'rules' key");
        return false;
    }

    m_classes = j["classes"].get<std::vector<std::string>>();
    const auto other = std::find(m_classes.begin(), m_classes.end(), "other");
    if (m_classes.empty() || other == m_classes.end()) {
        spdlog::error("ClassMapper: class list must contain 'other'");
        m_classes.clear();
        return false;
    }
    m_otherIdx = static_cast<int>(other - m_classes.begin());

    m_rules.clear();
    for (const auto& r : j["rules"]) {
        Rule rule;
        const std::string cls = r.value("class", "");
        const auto it = std::find(m_classes.begin(), m_classes.end(), cls);
        if (it == m_classes.end()) {
            spdlog::warn("ClassMapper: rule references unknown class '{}' — skipped", cls);
            continue;
        }
        rule.classIdx  = static_cast<int>(it - m_classes.begin());
        rule.footprint = r.value("footprint", "");
        rule.ref       = r.value("ref", "");
        rule.value     = r.value("value", "");
        m_rules.push_back(std::move(rule));
    }

    spdlog::info("ClassMapper: {} classes, {} rules loaded", m_classes.size(), m_rules.size());
    return true;
}

namespace {
bool regexHit(const std::string& pattern, const std::string& text)
{
    if (pattern.empty()) return true;  // unconstrained field
    try {
        const std::regex re(pattern, std::regex::icase);
        return std::regex_search(text, re);
    } catch (const std::regex_error& e) {
        spdlog::warn("ClassMapper: bad regex '{}': {}", pattern, e.what());
        return false;
    }
}
} // namespace

int ClassMapper::classId(const Component& c)
{
    for (const auto& rule : m_rules) {
        // A rule with all fields empty would match everything — guard against it.
        if (rule.footprint.empty() && rule.ref.empty() && rule.value.empty())
            continue;
        if (regexHit(rule.footprint, c.footprint) &&
            regexHit(rule.ref, c.reference) &&
            regexHit(rule.value, c.value)) {
            return rule.classIdx;
        }
    }
    if (m_unmapped.insert(c.footprint).second) {
        spdlog::warn("ClassMapper: no rule for footprint '{}' (ref {}, value '{}') "
                     "→ 'other'. Consider adding a rule to footprint_classes.json.",
                     c.footprint, c.reference, c.value);
    }
    return m_otherIdx;
}

// ============================================================================
// Label projection
// ============================================================================

std::vector<YoloLabel> projectLabels(const std::vector<Component>& components,
                                     Layer layer,
                                     const cv::Mat& H,
                                     cv::Size imageSize,
                                     const LabelParams& params,
                                     ClassMapper& mapper)
{
    std::vector<YoloLabel> labels;
    if (H.empty() || imageSize.width <= 0 || imageSize.height <= 0)
        return labels;

    const double imgW = imageSize.width;
    const double imgH = imageSize.height;

    for (const auto& comp : components) {
        if (comp.layer != layer) continue;
        if (comp.bbox.width() <= 0 || comp.bbox.height() <= 0) continue;

        // Project the 4 bbox corners → axis-aligned enclosing rect in the image.
        const std::vector<cv::Point2f> corners = {
            {static_cast<float>(comp.bbox.minX), static_cast<float>(comp.bbox.minY)},
            {static_cast<float>(comp.bbox.maxX), static_cast<float>(comp.bbox.minY)},
            {static_cast<float>(comp.bbox.maxX), static_cast<float>(comp.bbox.maxY)},
            {static_cast<float>(comp.bbox.minX), static_cast<float>(comp.bbox.maxY)}};
        std::vector<cv::Point2f> proj;
        cv::perspectiveTransform(corners, proj, H);

        double minX = proj[0].x, maxX = proj[0].x;
        double minY = proj[0].y, maxY = proj[0].y;
        for (const auto& p : proj) {
            minX = std::min<double>(minX, p.x);
            maxX = std::max<double>(maxX, p.x);
            minY = std::min<double>(minY, p.y);
            maxY = std::max<double>(maxY, p.y);
        }

        // Shrink around the center: the iBOM bbox often includes courtyard /
        // silkscreen, wider than the visible package.
        const double cx = (minX + maxX) / 2.0;
        const double cy = (minY + maxY) / 2.0;
        const double hw = (maxX - minX) / 2.0 * params.shrink;
        const double hh = (maxY - minY) / 2.0 * params.shrink;
        minX = cx - hw; maxX = cx + hw;
        minY = cy - hh; maxY = cy + hh;

        const double fullArea = (maxX - minX) * (maxY - minY);
        if (fullArea <= 0) continue;

        // Clip to the image; reject mostly-out-of-frame components.
        const double cMinX = std::clamp(minX, 0.0, imgW);
        const double cMaxX = std::clamp(maxX, 0.0, imgW);
        const double cMinY = std::clamp(minY, 0.0, imgH);
        const double cMaxY = std::clamp(maxY, 0.0, imgH);
        const double clipW = cMaxX - cMinX;
        const double clipH = cMaxY - cMinY;
        if (clipW <= 0 || clipH <= 0) continue;
        if ((clipW * clipH) / fullArea < params.minVisibleFrac) continue;
        if (clipW < params.minBoxPx || clipH < params.minBoxPx) continue;

        YoloLabel lbl;
        lbl.classId = mapper.classId(comp);
        lbl.cx = (cMinX + cMaxX) / 2.0 / imgW;
        lbl.cy = (cMinY + cMaxY) / 2.0 / imgH;
        lbl.w  = clipW / imgW;
        lbl.h  = clipH / imgH;
        labels.push_back(lbl);
    }
    return labels;
}

// ============================================================================
// DatasetCreator
// ============================================================================

DatasetCreator::DatasetCreator(QObject* parent)
    : QObject(parent)
{
}

void DatasetCreator::configure(int minInliers, double maxReprojErrPx, double minSharpness,
                               double maxBadExposureFrac, int maxHomographyAgeMs,
                               int saveIntervalMs, double minPoseDeltaPx,
                               double bboxShrink, int minBoxPx, double minVisibleFrac)
{
    m_minInliers         = std::max(4, minInliers);
    m_maxReprojErrPx     = maxReprojErrPx;
    m_minSharpness       = minSharpness;
    m_maxBadExposureFrac = std::clamp(maxBadExposureFrac, 0.0, 1.0);
    m_maxHomographyAgeMs = std::max(50, maxHomographyAgeMs);
    m_saveIntervalMs     = std::max(100, saveIntervalMs);
    m_minPoseDeltaPx     = std::max(0.0, minPoseDeltaPx);
    m_labelParams.shrink         = std::clamp(bboxShrink, 0.3, 1.0);
    m_labelParams.minBoxPx       = std::max(2, minBoxPx);
    m_labelParams.minVisibleFrac = std::clamp(minVisibleFrac, 0.0, 1.0);

    spdlog::info("DatasetCreator configured: inliers≥{}, reproj≤{:.1f}px, "
                 "sharp≥{:.0f}, badExp≤{:.2f}, age≤{}ms, interval={}ms, "
                 "poseΔ≥{:.0f}px, shrink={:.2f}",
                 m_minInliers, m_maxReprojErrPx, m_minSharpness,
                 m_maxBadExposureFrac, m_maxHomographyAgeMs, m_saveIntervalMs,
                 m_minPoseDeltaPx, m_labelParams.shrink);
}

void DatasetCreator::setProject(std::shared_ptr<const ibom::IBomProject> project, ibom::Layer layer)
{
    m_project = std::move(project);
    m_layer   = layer;
}

void DatasetCreator::setClassRulesPath(QString path)
{
    m_classRulesPath = std::move(path);
}

void DatasetCreator::setOutputRoot(QString root)
{
    m_outputRoot = std::move(root);
}

void DatasetCreator::startSession(QString boardName, QString lightingTag)
{
    if (m_active) {
        emit sessionError(tr("A capture session is already running"));
        return;
    }
    if (!m_project || m_project->components.empty()) {
        emit sessionError(tr("No iBOM loaded — open an iBOM file first"));
        return;
    }
    if (!m_mapper.isLoaded() && !m_mapper.load(m_classRulesPath.toStdString())) {
        emit sessionError(tr("Cannot load class rules (%1)").arg(m_classRulesPath));
        return;
    }
    if (m_outputRoot.isEmpty()) {
        emit sessionError(tr("Dataset output directory not configured"));
        return;
    }

    // Sanitize free-text tags for use in a directory name.
    auto slug = [](QString s) {
        s = s.trimmed().toLower();
        s.replace(QRegularExpression("[^a-z0-9]+"), "-");
        s.replace(QRegularExpression("(^-+|-+$)"), "");
        return s.isEmpty() ? QString("unnamed") : s;
    };
    m_boardName   = slug(boardName);
    m_lightingTag = slug(lightingTag);

    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmmss");
    m_sessionDir = fs::path(m_outputRoot.toStdString()) /
                   QString("session_%1_%2_%3").arg(stamp, m_boardName, m_lightingTag)
                       .toStdString();

    try {
        fs::create_directories(m_sessionDir / "images");
        fs::create_directories(m_sessionDir / "labels");
    } catch (const fs::filesystem_error& e) {
        emit sessionError(tr("Cannot create session directory: %1").arg(e.what()));
        return;
    }

    m_manifest.open(m_sessionDir / "manifest.jsonl", std::ios::out | std::ios::trunc);
    if (!m_manifest) {
        emit sessionError(tr("Cannot create manifest.jsonl"));
        return;
    }

    m_frameIndex = 0;
    m_lastSavedPose = cv::Mat();
    m_status = DatasetStatus{};
    m_status.sessionActive = true;
    m_active = true;

    spdlog::info("DatasetCreator: session started → {}", m_sessionDir.string());
    emit sessionStarted(QString::fromStdString(m_sessionDir.string()));
}

void DatasetCreator::stopSession()
{
    if (!m_active) return;
    m_active = false;
    m_manifest.close();

    // Surface the unmapped footprints at end of session — this is the moment
    // the user curates footprint_classes.json.
    if (!m_mapper.unmappedFootprints().empty()) {
        std::string list;
        for (const auto& fp : m_mapper.unmappedFootprints())
            list += "\n  - " + fp;
        spdlog::warn("DatasetCreator: {} footprint(s) had no mapping rule:{}",
                     m_mapper.unmappedFootprints().size(), list);
    }

    spdlog::info("DatasetCreator: session stopped — {} image(s) saved in {}",
                 m_status.saved, m_sessionDir.string());
    m_status.sessionActive = false;
    emit statusUpdated(m_status);
    emit sessionStopped(m_status.saved);
}

void DatasetCreator::onHomography(cv::Mat h, int inliers, double reprojErrPx)
{
    m_homography  = std::move(h);
    m_inliers     = inliers;
    m_reprojErrPx = reprojErrPx;
    m_lastHomographyTime = std::chrono::steady_clock::now();
}

bool DatasetCreator::evaluateGates(const cv::Mat& gray, DatasetStatus& st) const
{
    using ms = std::chrono::duration<double, std::milli>;
    st.inliers         = m_inliers;
    st.reprojErrPx     = m_reprojErrPx;
    st.homographyAgeMs = ms(std::chrono::steady_clock::now() - m_lastHomographyTime).count();

    st.gateTracking = m_inliers >= m_minInliers;
    st.gateReproj   = st.gateTracking && m_reprojErrPx <= m_maxReprojErrPx;
    st.gateFresh    = !m_homography.empty() && st.homographyAgeMs <= m_maxHomographyAgeMs;

    // Sharpness: Laplacian variance on a downscaled grayscale (cheap, stable).
    cv::Mat small;
    cv::resize(gray, small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);
    cv::Mat lap;
    cv::Laplacian(small, lap, CV_64F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    st.sharpness     = stddev[0] * stddev[0];
    st.gateSharpness = st.sharpness >= m_minSharpness;

    // Exposure: fraction of pixels crushed to black or blown to white.
    const int total = small.rows * small.cols;
    const int black = cv::countNonZero(small < 5);
    const int white = cv::countNonZero(small > 250);
    st.badExposureFrac = total > 0 ? static_cast<double>(black + white) / total : 1.0;
    st.gateExposure    = st.badExposureFrac <= m_maxBadExposureFrac;

    return st.gateTracking && st.gateReproj && st.gateSharpness &&
           st.gateExposure && st.gateFresh;
}

bool DatasetCreator::poseMovedEnough(const cv::Mat& h) const
{
    if (m_lastSavedPose.empty()) return true;
    if (!m_project) return false;

    // Compare the projected board-bbox corners between the last saved pose
    // and the current one — viewpoint must have actually changed.
    const auto& bb = m_project->boardInfo.boardBBox;
    const std::vector<cv::Point2f> corners = {
        {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
        {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
        {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
        {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}};
    std::vector<cv::Point2f> now, before;
    cv::perspectiveTransform(corners, now, h);
    cv::perspectiveTransform(corners, before, m_lastSavedPose);

    double meanDelta = 0.0;
    for (size_t i = 0; i < corners.size(); ++i) {
        const cv::Point2f d = now[i] - before[i];
        meanDelta += std::hypot(static_cast<double>(d.x), static_cast<double>(d.y));
    }
    meanDelta /= static_cast<double>(corners.size());
    return meanDelta >= m_minPoseDeltaPx;
}

void DatasetCreator::processFrame(ibom::camera::FrameRef frame)
{
    if (!m_active || !frame || frame->empty())
        return;

    // Throttle the whole evaluation (gates included) — no need to grade
    // 30 fps when we save at most 2 img/s.
    const auto now = std::chrono::steady_clock::now();
    const auto sinceStatus = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastStatusTime).count();
    if (sinceStatus < 200) return;
    m_lastStatusTime = now;

    cv::Mat gray;
    if (frame->channels() == 3)
        cv::cvtColor(*frame, gray, cv::COLOR_BGR2GRAY);
    else
        gray = *frame;

    const bool gatesOk = evaluateGates(gray, m_status);
    if (!gatesOk) {
        ++m_status.rejectedGates;
        emit statusUpdated(m_status);
        return;
    }

    const auto sinceSave = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastSaveTime).count();
    if (sinceSave < m_saveIntervalMs) {
        emit statusUpdated(m_status);  // gates green, just too soon — not a rejection
        return;
    }

    if (!poseMovedEnough(m_homography)) {
        ++m_status.rejectedPose;
        emit statusUpdated(m_status);
        return;
    }

    const auto labels = projectLabels(m_project->components, m_layer, m_homography,
                                      cv::Size(frame->cols, frame->rows),
                                      m_labelParams, m_mapper);
    m_status.lastLabelCount = static_cast<int>(labels.size());
    if (labels.empty()) {
        ++m_status.rejectedLabels;
        emit statusUpdated(m_status);
        return;
    }

    writeFrame(*frame, labels, m_status);
    m_lastSaveTime  = now;
    m_lastSavedPose = m_homography.clone();
    ++m_status.saved;
    emit statusUpdated(m_status);
}

void DatasetCreator::writeFrame(const cv::Mat& frame, const std::vector<YoloLabel>& labels,
                                const DatasetStatus& st)
{
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06d", m_frameIndex++);

    const fs::path imgPath = m_sessionDir / "images" / (std::string(name) + ".jpg");
    const fs::path lblPath = m_sessionDir / "labels" / (std::string(name) + ".txt");

    if (!cv::imwrite(imgPath.string(), frame, {cv::IMWRITE_JPEG_QUALITY, 95})) {
        spdlog::error("DatasetCreator: failed to write {}", imgPath.string());
        return;
    }

    std::ofstream lbl(lblPath);
    for (const auto& l : labels) {
        char line[128];
        std::snprintf(line, sizeof(line), "%d %.6f %.6f %.6f %.6f\n",
                      l.classId, l.cx, l.cy, l.w, l.h);
        lbl << line;
    }

    // Manifest: one JSON line per saved frame — feeds the Studio's diversity
    // checks and the future stratified split.
    nlohmann::json m;
    m["frame"]      = std::string(name) + ".jpg";
    m["board"]      = m_boardName.toStdString();
    m["lighting"]   = m_lightingTag.toStdString();
    m["labels"]     = labels.size();
    m["inliers"]    = st.inliers;
    m["reproj_err"] = st.reprojErrPx;
    m["sharpness"]  = st.sharpness;
    std::vector<double> hFlat(9, 0.0);
    if (m_homography.type() == CV_64F && m_homography.total() == 9)
        hFlat.assign(reinterpret_cast<const double*>(m_homography.data),
                     reinterpret_cast<const double*>(m_homography.data) + 9);
    m["homography"] = hFlat;
    m_manifest << m.dump() << "\n";
    m_manifest.flush();
}

} // namespace ibom::features
