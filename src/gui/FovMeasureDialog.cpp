#include "FovMeasureDialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QGroupBox>
#include <QPushButton>
#include <QFrame>
#include <QTextEdit>
#include <QDialogButtonBox>
#include <QClipboard>
#include <QApplication>
#include <cmath>

namespace ibom::gui {

static QLabel* valueLabel(const QString& text, bool warn = false)
{
    auto* l = new QLabel(text);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (warn)
        l->setStyleSheet("color: #ffa500; font-weight: bold;");
    return l;
}

FovMeasureDialog::FovMeasureDialog(const Metrics& m, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("FOV & Scale Measurement — Dev"));
    setMinimumWidth(480);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(12);

    // ── Camera & Profile ─────────────────────────────────────────────
    {
        auto* grp  = new QGroupBox(tr("Camera"));
        auto* form = new QFormLayout(grp);
        form->addRow(tr("Profile:"),    new QLabel(m.profileName));
        form->addRow(tr("Backend:"),    new QLabel(m.backendName));
        form->addRow(tr("Resolution:"), new QLabel(
            tr("%1 × %2 px").arg(m.camWidth).arg(m.camHeight)));

        const QString calibStr = m.calibrated
            ? tr("Loaded  (RMS: %1 px)").arg(m.calibRmsErr, 0, 'f', 3)
            : tr("NOT loaded — undistort disabled");
        form->addRow(tr("Calibration:"), valueLabel(calibStr, !m.calibrated));
        root->addWidget(grp);
    }

    // ── Scale & FOV ──────────────────────────────────────────────────
    {
        auto* grp  = new QGroupBox(tr("Scale & Field of View"));
        auto* form = new QFormLayout(grp);

        const bool hasScale = m.pixelsPerMm > 0.0;

        const QString scaleStr = hasScale
            ? tr("%1 px/mm  (%2)").arg(m.pixelsPerMm, 0, 'f', 2).arg(m.scaleSource)
            : tr("Unknown — anchor or align the overlay first");
        form->addRow(tr("Scale:"), valueLabel(scaleStr, !hasScale));

        if (hasScale) {
            form->addRow(tr("FOV width:"),
                valueLabel(tr("%1 mm").arg(m.fovWidthMm, 0, 'f', 1)));
            form->addRow(tr("FOV height:"),
                valueLabel(tr("%1 mm").arg(m.fovHeightMm, 0, 'f', 1)));
        }

        // Components in FOV
        if (m.componentsInFov >= 0) {
            const bool fewComps = m.componentsInFov < 3;
            form->addRow(tr("Components in FOV:"),
                valueLabel(tr("%1 of %2 total").arg(m.componentsInFov)
                                                .arg(m.totalComponents), fewComps));
            if (fewComps)
                form->addRow(new QLabel(tr(
                    "⚠  < 3 components visible: incremental tracking recommended\n"
                    "    (see Settings → Tracking → Incremental frame→frame)")));
        } else {
            form->addRow(tr("Components in FOV:"),
                new QLabel(tr("N/A (no homography or iBOM)")));
        }

        root->addWidget(grp);
    }

    // ── Config Recommendations ───────────────────────────────────────
    {
        auto* grp  = new QGroupBox(tr("Config Recommendations"));
        auto* form = new QFormLayout(grp);

        if (m.pixelsPerMm > 0.0) {
            const double diff = m.pixelsPerMm - m.configAnchorPxPerMm;
            const bool changed = std::abs(diff) > 0.5;
            const QString curStr = tr("%1 px/mm").arg(m.configAnchorPxPerMm, 0, 'f', 2);
            form->addRow(tr("anchor_pixels_per_mm (stored):"), new QLabel(curStr));

            const QString recStr = tr("%1 px/mm  (%2)")
                .arg(m.pixelsPerMm, 0, 'f', 2)
                .arg(diff >= 0 ? tr("+%1").arg(diff, 0, 'f', 1)
                               : tr("%1").arg(diff, 0, 'f', 1));
            form->addRow(tr("Recommended value:"), valueLabel(recStr, changed));

            if (changed)
                form->addRow(new QLabel(tr(
                    "Update Settings → Camera (microscope section)\n"
                    "  Anchor px/mm = %1\n"
                    "so the first anchor gets the right scale bootstrap.").arg(
                    m.pixelsPerMm, 0, 'f', 1)));
            else
                form->addRow(new QLabel(tr("✓ Stored value is close — no change needed.")));
        } else {
            form->addRow(new QLabel(tr(
                "Anchor or align the overlay to get a live scale reading,\n"
                "then re-open this dialog.")));
        }

        root->addWidget(grp);
    }

    // ── Script instructions ──────────────────────────────────────────
    {
        auto* grp  = new QGroupBox(tr("Off-line measurement (scripts/measure_fov.py)"));
        auto* info = new QLabel(tr(
            "For an automated measurement without the overlay:\n\n"
            "  # Microscope (V4L2 index 0), 7×5 checkerboard, 5 mm squares:\n"
            "  python3 scripts/measure_fov.py \\\n"
            "      --camera v4l2 --device 0 \\\n"
            "      --calibration <AppData>/calibration.yml \\\n"
            "      --checkerboard 7 5 --square-size 5.0\n\n"
            "  # RealSense D405:\n"
            "  python3 scripts/measure_fov.py --camera realsense\n\n"
            "The script captures 10 frames, detects the checkerboard, computes\n"
            "px/mm and FOV, and writes a JSON report + logs to stdout."));
        info->setWordWrap(true);
        info->setTextInteractionFlags(Qt::TextSelectableByMouse);
        info->setFont(QFont("monospace", 9));
        auto* vl = new QVBoxLayout(grp);
        vl->addWidget(info);
        root->addWidget(grp);
    }

    // ── Buttons ───────────────────────────────────────────────────────
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::accept);
    root->addWidget(bb);
}

} // namespace ibom::gui
