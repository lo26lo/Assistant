#include "CalibrationMonitorDialog.h"

#include "../app/Config.h"
#include "../utils/ImageUtils.h"
#include "../utils/QtLogSink.h"
#include "../utils/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QClipboard>
#include <QApplication>
#include <QDateTime>
#include <QTimer>
#include <QFont>
#include <QPixmap>
#include <QtConcurrent/QtConcurrent>

#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

#include <algorithm>
#include <cmath>

namespace ibom::gui {

namespace {
constexpr int   kPreviewWidth   = 360;   // px
constexpr int   kDetectMaxWidth = 1024;  // downscale detection above this
constexpr int   kThrottleMs     = 180;   // detection cadence
constexpr int   kMaxLogLines    = 300;
constexpr double kSharpScale    = 0.25;  // matches the app's focus-assist metric

QString styleFor(const QString& kind)
{
    if (kind == "good")  return "color:#a6e3a1; font-weight:bold;";   // green
    if (kind == "warn")  return "color:#f9e2af; font-weight:bold;";   // amber
    if (kind == "bad")   return "color:#f38ba8; font-weight:bold;";   // red
    return "color:#cdd6f4;";                                          // neutral
}

// Runs on a QtConcurrent worker thread: no widget access, pure pixel work.
// findChessboardCornersSB can take well over kThrottleMs to give up on a
// frame with no board in it — keeping this off the GUI thread is what stops
// the whole application from freezing while the dialog is open.
CalibDetectionResult computeDetection(const cv::Mat& frame, int cols, int rows)
{
    CalibDetectionResult r;
    r.cornersExpected = cols * rows;
    const cv::Size board(cols, rows);

    cv::Mat gray;
    if (frame.channels() == 3)
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    else
        gray = frame;

    const int fullW = gray.cols;

    double ds = 1.0;
    cv::Mat det = gray;
    if (gray.cols > kDetectMaxWidth) {
        ds = static_cast<double>(kDetectMaxWidth) / gray.cols;
        cv::resize(gray, det, cv::Size(), ds, ds, cv::INTER_AREA);
    }

    std::vector<cv::Point2f> corners;
    bool found = false;
    r.method = QObject::tr("not found");
    try {
        found = cv::findChessboardCornersSB(
            det, board, corners,
            cv::CALIB_CB_NORMALIZE_IMAGE | cv::CALIB_CB_ACCURACY);
        if (found) {
            r.method = QObject::tr("sector-based (SB)");
        } else {
            found = cv::findChessboardCorners(
                det, board, corners,
                cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE
                    | cv::CALIB_CB_FAST_CHECK);
            if (found) r.method = QObject::tr("legacy adaptive");
        }
    } catch (const cv::Exception&) {
        found = false;
    }

    r.detected     = found;
    r.cornersFound = found ? static_cast<int>(corners.size()) : 0;

    cv::Mat smallGray;
    cv::resize(gray, smallGray, cv::Size(), kSharpScale, kSharpScale, cv::INTER_AREA);
    r.sharpness  = utils::ImageUtils::computeSharpness(smallGray);
    r.brightness = cv::mean(gray)[0];

    if (found && !corners.empty()) {
        const cv::Rect bb = cv::boundingRect(corners);
        r.coveragePct = 100.0 * (static_cast<double>(bb.width) * bb.height)
                              / (static_cast<double>(det.cols) * det.rows);
        const double cx = bb.x + bb.width / 2.0;
        const double cy = bb.y + bb.height / 2.0;
        const QString vside = (cy < det.rows / 3.0) ? QObject::tr("top")
                            : (cy > det.rows * 2.0 / 3.0) ? QObject::tr("bottom") : QObject::tr("middle");
        const QString hside = (cx < det.cols / 3.0) ? QObject::tr("left")
                            : (cx > det.cols * 2.0 / 3.0) ? QObject::tr("right") : QObject::tr("centre");
        r.quadrant = (vside == QObject::tr("middle") && hside == QObject::tr("centre"))
            ? QObject::tr("centre") : QObject::tr("%1-%2").arg(vside, hside);
    }

    cv::Mat previewBgr;
    if (frame.channels() == 3)
        previewBgr = frame.clone();
    else
        cv::cvtColor(frame, previewBgr, cv::COLOR_GRAY2BGR);

    const double pf = static_cast<double>(kPreviewWidth) / fullW;
    cv::resize(previewBgr, previewBgr, cv::Size(), pf, pf, cv::INTER_AREA);
    if (found && !corners.empty()) {
        std::vector<cv::Point2f> pc;
        pc.reserve(corners.size());
        const double k = pf / ds;
        for (const auto& p : corners)
            pc.emplace_back(p.x * k, p.y * k);
        cv::drawChessboardCorners(previewBgr, board, pc, found);
    }
    r.preview = utils::ImageUtils::matToQImage(previewBgr).copy();

    return r;
}
} // namespace

CalibrationMonitorDialog::CalibrationMonitorDialog(const ibom::Config& config,
                                                   QWidget* parent)
    : QDialog(parent)
    , m_config(config)
{
    setWindowTitle(tr("Calibration Monitor — Dev (live)"));
    setMinimumSize(720, 640);
    // Non-modal: the user keeps clicking "Calibrate" on the main window while
    // watching this. The caller shows it with show(), not exec().
    setModal(false);

    buildUi();
    resize(820, 720);
    m_throttle.start();

    m_watcher = new QFutureWatcher<CalibDetectionResult>(this);
    connect(m_watcher, &QFutureWatcher<CalibDetectionResult>::finished,
            this, &CalibrationMonitorDialog::onDetectionFinished);

    // Live problem feed. Records arrive from any thread; Queued marshals onto
    // the GUI thread. Connected for the dialog's whole lifetime so that recent
    // warnings are already present the moment it is shown.
    connect(&utils::LogBridge::instance(), &utils::LogBridge::messageLogged,
            this, &CalibrationMonitorDialog::appendLog, Qt::QueuedConnection);

    updateVerdict();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void CalibrationMonitorDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    // ── Readiness banner ─────────────────────────────────────────────
    m_verdict = new QLabel;
    m_verdict->setWordWrap(true);
    m_verdict->setAlignment(Qt::AlignCenter);
    {
        QFont f = m_verdict->font();
        f.setPointSize(f.pointSize() + 1);
        m_verdict->setFont(f);
    }
    root->addWidget(m_verdict);

    // ── Preview + live metrics, side by side ─────────────────────────
    auto* mid = new QHBoxLayout;

    m_preview = new QLabel;
    m_preview->setFixedWidth(kPreviewWidth);
    m_preview->setMinimumHeight(kPreviewWidth * 9 / 16);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet("background:#11111b; border:1px solid #313244;");
    m_preview->setText(tr("(no frame yet —\nstart the camera)"));
    mid->addWidget(m_preview);

    auto* liveGrp  = new QGroupBox(tr("Live frame"));
    liveGrp->setMinimumWidth(320);
    auto* liveForm = new QFormLayout(liveGrp);
    liveForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_lblDetect   = new QLabel; m_lblDetect->setWordWrap(true);   liveForm->addRow(tr("Checkerboard:"), m_lblDetect);
    m_lblSharp    = new QLabel; m_lblSharp->setWordWrap(true);    liveForm->addRow(tr("Sharpness:"),    m_lblSharp);
    m_lblCoverage = new QLabel; m_lblCoverage->setWordWrap(true); liveForm->addRow(tr("Board area:"),   m_lblCoverage);
    m_lblBright   = new QLabel; m_lblBright->setWordWrap(true);   liveForm->addRow(tr("Brightness:"),   m_lblBright);
    m_lblParams   = new QLabel; m_lblParams->setWordWrap(true);
    liveForm->addRow(tr("Pattern:"), m_lblParams);
    mid->addWidget(liveGrp, 1);

    root->addLayout(mid);

    // ── Calibration state ────────────────────────────────────────────
    auto* calibGrp  = new QGroupBox(tr("Calibration state"));
    auto* calibForm = new QFormLayout(calibGrp);
    m_lblCalib    = new QLabel; m_lblCalib->setWordWrap(true);
    calibForm->addRow(tr("Camera / file:"), m_lblCalib);
    m_lblScale    = new QLabel;
    calibForm->addRow(tr("Scale / RMS:"),   m_lblScale);
    m_lblProgress = new QLabel;
    calibForm->addRow(tr("Capture run:"),   m_lblProgress);
    root->addWidget(calibGrp);

    // ── Problem log ──────────────────────────────────────────────────
    auto* logGrp = new QGroupBox(tr("Problems (live WARN / ERROR)"));
    auto* logLay = new QVBoxLayout(logGrp);
    m_logView = new QTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setMinimumHeight(120);
    m_logView->document()->setMaximumBlockCount(kMaxLogLines);
    m_logView->setStyleSheet("background:#11111b;");
    m_logView->setFont(QFont("monospace", 9));
    m_logView->setPlaceholderText(tr("No warnings yet — calibration issues will appear here."));
    logLay->addWidget(m_logView);
    root->addWidget(logGrp, 1);

    // ── Buttons ──────────────────────────────────────────────────────
    auto* btnRow  = new QHBoxLayout;
    auto* capBtn  = new QPushButton(tr("Capture image"));
    capBtn->setToolTip(tr("Grab one calibration frame — same as the main "
                          "Calibrate button. Take 5 at different angles."));
    connect(capBtn, &QPushButton::clicked, this,
            &CalibrationMonitorDialog::captureRequested);
    btnRow->addWidget(capBtn);

    m_copyBtn = new QPushButton(tr("Copy report"));
    m_copyBtn->setToolTip(tr("Copy a full text snapshot (state + recent problems) "
                             "to the clipboard so it can be pasted for diagnosis."));
    connect(m_copyBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(buildReport());
        const QString prev = tr("Copy report");
        m_copyBtn->setText(tr("Copied ✓"));
        QTimer::singleShot(1500, this, [this, prev]() {
            if (m_copyBtn) m_copyBtn->setText(prev);
        });
    });
    btnRow->addWidget(m_copyBtn);

    auto* clearBtn = new QPushButton(tr("Clear log"));
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        m_logLines.clear();
        if (m_logView) m_logView->clear();
    });
    btnRow->addWidget(clearBtn);

    btnRow->addStretch(1);
    auto* closeBtn = new QPushButton(tr("Close"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::hide);
    btnRow->addWidget(closeBtn);

    root->addLayout(btnRow);
}

// ---------------------------------------------------------------------------
// Frame feed + detection
// ---------------------------------------------------------------------------

void CalibrationMonitorDialog::onFrame(const ibom::camera::FrameRef& frame)
{
    if (!frame || frame->empty()) return;
    if (m_throttle.elapsed() < kThrottleMs) return;
    if (m_detectionBusy) return;   // previous pass still running — drop this frame
    m_throttle.restart();
    m_detectionBusy = true;
    m_haveFrame = true;

    const cv::Mat clone = frame->clone();
    const int cols = m_config.calibBoardCols();
    const int rows = m_config.calibBoardRows();
    m_watcher->setFuture(QtConcurrent::run(
        [clone, cols, rows]() { return computeDetection(clone, cols, rows); }));
}

void CalibrationMonitorDialog::onDetectionFinished()
{
    m_detectionBusy = false;
    const CalibDetectionResult res = m_watcher->result();

    m_detected        = res.detected;
    m_cornersFound     = res.cornersFound;
    m_cornersExpected  = res.cornersExpected;
    m_method           = res.method;
    m_sharpness        = res.sharpness;
    m_brightness       = res.brightness;
    m_coveragePct      = res.coveragePct;
    m_quadrant         = res.quadrant;

    if (!res.preview.isNull())
        m_preview->setPixmap(QPixmap::fromImage(res.preview));

    if (res.detected) {
        const bool full = (m_cornersFound == m_cornersExpected);
        m_lblDetect->setText(tr("✓ found %1/%2 corners (%3)")
            .arg(m_cornersFound).arg(m_cornersExpected).arg(m_method));
        m_lblDetect->setStyleSheet(styleFor(full ? "good" : "warn"));
    } else {
        m_lblDetect->setText(tr("✗ not detected (expected %1)").arg(m_cornersExpected));
        m_lblDetect->setStyleSheet(styleFor("bad"));
    }

    const double sharpMin = m_config.datasetMinSharpness();
    const bool   sharpOk  = m_sharpness >= sharpMin;
    m_lblSharp->setText(tr("%1  (min %2) — %3")
        .arg(m_sharpness, 0, 'f', 0).arg(sharpMin, 0, 'f', 0)
        .arg(sharpOk ? tr("sharp") : tr("BLURRY")));
    m_lblSharp->setStyleSheet(styleFor(sharpOk ? "good" : "warn"));

    if (res.detected) {
        const bool covOk = m_coveragePct >= 15.0 && m_coveragePct <= 90.0;
        m_lblCoverage->setText(tr("%1% of frame · %2")
            .arg(m_coveragePct, 0, 'f', 0).arg(m_quadrant));
        m_lblCoverage->setStyleSheet(styleFor(covOk ? "good" : "warn"));
    } else {
        m_lblCoverage->setText(tr("—"));
        m_lblCoverage->setStyleSheet(styleFor("neutral"));
    }

    QString brightKind = "good";
    QString brightNote = tr("ok");
    if (m_brightness < 40)        { brightKind = "warn"; brightNote = tr("too dark"); }
    else if (m_brightness > 220)  { brightKind = "warn"; brightNote = tr("too bright / glare"); }
    m_lblBright->setText(tr("mean %1 — %2").arg(m_brightness, 0, 'f', 0).arg(brightNote));
    m_lblBright->setStyleSheet(styleFor(brightKind));

    const int cols = m_config.calibBoardCols();
    const int rows = m_config.calibBoardRows();
    const float sqmm = m_config.calibSquareSize();
    m_lblParams->setText(tr("%1 × %2 inner corners · %3 mm squares · %4 expected")
        .arg(cols).arg(rows).arg(sqmm, 0, 'f', 1).arg(m_cornersExpected));

    updateVerdict();
}

// ---------------------------------------------------------------------------
// Pushed state
// ---------------------------------------------------------------------------

void CalibrationMonitorDialog::setCalibrationStatus(
    bool calibrated, double rmsErr, double pixelsPerMm,
    const QString& backendName, int camW, int camH, const QString& calibFilePath)
{
    m_calibrated    = calibrated;
    m_rmsErr        = rmsErr;
    m_pixelsPerMm   = pixelsPerMm;
    m_backendName   = backendName;
    m_camW          = camW;
    m_camH          = camH;
    m_calibFilePath = calibFilePath;

    QString calibState;
    QString kind = "neutral";
    if (calibrated) {
        const bool goodRms = rmsErr > 0.0 && rmsErr <= 1.0;
        calibState = tr("loaded (RMS %1 px)").arg(rmsErr, 0, 'f', 3);
        kind = goodRms ? "good" : "warn";
    } else {
        calibState = tr("NOT calibrated — undistort disabled");
        kind = "bad";
    }
    m_lblCalib->setText(tr("%1 · %2×%3\n%4\n%5")
        .arg(backendName).arg(camW).arg(camH).arg(calibState)
        .arg(calibFilePath.isEmpty() ? tr("(no file)") : calibFilePath));
    m_lblCalib->setStyleSheet(styleFor(kind));

    if (pixelsPerMm > 0.0 && camW > 0) {
        const double fovW = camW / pixelsPerMm;
        const double fovH = camH / pixelsPerMm;
        m_lblScale->setText(tr("%1 px/mm · FOV %2 × %3 mm · RMS %4 px")
            .arg(pixelsPerMm, 0, 'f', 2).arg(fovW, 0, 'f', 1)
            .arg(fovH, 0, 'f', 1).arg(rmsErr, 0, 'f', 3));
    } else {
        m_lblScale->setText(tr("scale unknown — calibrate or anchor first"));
    }

    updateVerdict();
}

void CalibrationMonitorDialog::setCaptureProgress(bool collecting, int captured, int total)
{
    m_collecting   = collecting;
    m_captured     = captured;
    m_captureTotal = total > 0 ? total : 5;

    if (collecting)
        m_lblProgress->setText(tr("collecting — %1 / %2 captured")
            .arg(captured).arg(m_captureTotal));
    else if (captured > 0)
        m_lblProgress->setText(tr("last run: %1 / %2 captured")
            .arg(captured).arg(m_captureTotal));
    else
        m_lblProgress->setText(tr("idle — click \"Capture image\" to start"));

    updateVerdict();
}

// ---------------------------------------------------------------------------
// Verdict banner
// ---------------------------------------------------------------------------

void CalibrationMonitorDialog::updateVerdict()
{
    QString text, kind;
    const double sharpMin = m_config.datasetMinSharpness();

    if (!m_haveFrame) {
        text = tr("Waiting for camera frames — start the camera.");
        kind = "neutral";
    } else if (!m_detected) {
        text = tr("✗ Checkerboard not detected — fill more of the frame, hold it flat, improve lighting.");
        kind = "bad";
    } else if (m_sharpness < sharpMin) {
        text = tr("Detected but BLURRY — adjust focus before capturing.");
        kind = "warn";
    } else if (m_coveragePct < 15.0) {
        text = tr("Detected — board too small/far. Move it closer to fill the frame.");
        kind = "warn";
    } else {
        text = tr("READY ✓ — detected & sharp. Capture, then tilt the board to a new angle.");
        kind = "good";
    }

    if (m_collecting)
        text += tr("   ·   capturing %1/%2").arg(m_captured).arg(m_captureTotal);

    m_verdict->setText(text);
    m_verdict->setStyleSheet(styleFor(kind) + " padding:6px; border-radius:4px;");
}

// ---------------------------------------------------------------------------
// Live problem log
// ---------------------------------------------------------------------------

void CalibrationMonitorDialog::appendLog(int level, const QString& logger,
                                         const QString& message)
{
    if (level < 3) return;   // keep WARN(3)/ERR(4)/CRIT(5) only

    const QString ts   = QDateTime::currentDateTime().toString("HH:mm:ss");
    const QString name = (level >= 5) ? "CRIT" : (level == 4 ? "ERR" : "WARN");
    const QString line = QString("[%1] %2 %3%4")
        .arg(ts, name,
             logger.isEmpty() ? QString() : (logger + ": "),
             message);

    m_logLines.append(line);
    while (m_logLines.size() > kMaxLogLines)
        m_logLines.removeFirst();

    if (m_logView) {
        const QString color = (level >= 4) ? "#f38ba8" : "#f9e2af";
        m_logView->append(QString("<span style='color:%1'>%2</span>")
            .arg(color, line.toHtmlEscaped()));
    }
}

// ---------------------------------------------------------------------------
// Clipboard report
// ---------------------------------------------------------------------------

QString CalibrationMonitorDialog::buildReport() const
{
    const int cols = m_config.calibBoardCols();
    const int rows = m_config.calibBoardRows();

    QString r;
    r += "=== MicroscopeIBOM — Calibration Monitor report ===\n";
    r += QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "\n\n";

    r += "[Camera / calibration]\n";
    r += QString("  Backend       : %1\n").arg(m_backendName.isEmpty() ? "(unknown)" : m_backendName);
    r += QString("  Resolution    : %1 x %2\n").arg(m_camW).arg(m_camH);
    r += QString("  Calibrated    : %1\n").arg(m_calibrated ? "yes" : "no");
    r += QString("  RMS error     : %1 px\n").arg(m_rmsErr, 0, 'f', 4);
    r += QString("  Pixels per mm : %1\n").arg(m_pixelsPerMm, 0, 'f', 3);
    if (m_pixelsPerMm > 0.0 && m_camW > 0)
        r += QString("  FOV           : %1 x %2 mm\n")
                .arg(m_camW / m_pixelsPerMm, 0, 'f', 1)
                .arg(m_camH / m_pixelsPerMm, 0, 'f', 1);
    r += QString("  Calib file    : %1\n\n").arg(m_calibFilePath.isEmpty() ? "(none)" : m_calibFilePath);

    r += "[Checkerboard pattern]\n";
    r += QString("  Inner corners : %1 x %2 (%3 expected)\n").arg(cols).arg(rows).arg(cols * rows);
    r += QString("  Square size   : %1 mm\n\n").arg(m_config.calibSquareSize(), 0, 'f', 2);

    r += "[Live frame]\n";
    r += QString("  Frame seen    : %1\n").arg(m_haveFrame ? "yes" : "no");
    r += QString("  Detected      : %1 (%2)\n").arg(m_detected ? "yes" : "no", m_method);
    r += QString("  Corners found : %1 / %2\n").arg(m_cornersFound).arg(m_cornersExpected);
    r += QString("  Sharpness     : %1 (min %2 -> %3)\n")
            .arg(m_sharpness, 0, 'f', 0)
            .arg(m_config.datasetMinSharpness(), 0, 'f', 0)
            .arg(m_sharpness >= m_config.datasetMinSharpness() ? "sharp" : "BLURRY");
    r += QString("  Board area    : %1 %% of frame (%2)\n").arg(m_coveragePct, 0, 'f', 0).arg(m_quadrant);
    r += QString("  Brightness    : %1 (mean)\n\n").arg(m_brightness, 0, 'f', 0);

    r += "[Capture run]\n";
    r += QString("  Collecting    : %1\n").arg(m_collecting ? "yes" : "no");
    r += QString("  Captured      : %1 / %2\n\n").arg(m_captured).arg(m_captureTotal);

    r += QString("[Recent problems — %1 line(s)]\n").arg(m_logLines.size());
    if (m_logLines.isEmpty())
        r += "  (none)\n";
    else
        for (const auto& l : m_logLines)
            r += "  " + l + "\n";

    r += QString("\nFull log file: %1\n")
            .arg(QString::fromStdString(utils::Logger::logFilePath()));
    return r;
}

} // namespace ibom::gui
