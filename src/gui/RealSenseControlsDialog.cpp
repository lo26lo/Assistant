#include "RealSenseControlsDialog.h"
#include "../camera/RealSenseCapture.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QScroller>
#include <QLabel>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QDialogButtonBox>

#include <map>

namespace ibom::gui {

RealSenseControlsDialog::RealSenseControlsDialog(camera::RealSenseCapture* camera,
                                                 QWidget* parent)
    : QDialog(parent)
    , m_camera(camera)
{
    setWindowTitle(tr("RealSense — Camera Controls"));
    setMinimumSize(440, 560);

    auto* root = new QVBoxLayout(this);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    QScroller::grabGesture(m_scroll->viewport(), QScroller::TouchGesture);
    root->addWidget(m_scroll, 1);

    auto* buttons = new QDialogButtonBox;
    auto* refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(refresh, &QPushButton::clicked, this, &RealSenseControlsDialog::rebuild);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    rebuild();
}

void RealSenseControlsDialog::rebuild()
{
    // Replace the content widget wholesale — simplest way to clear old controls.
    auto* fresh = new QWidget;
    auto* outer = new QVBoxLayout(fresh);

    const auto controls = m_camera ? m_camera->listControls()
                                   : std::vector<camera::RsControl>{};

    if (controls.empty()) {
        outer->addWidget(new QLabel(
            tr("No RealSense options available.\n"
               "Start the RealSense backend with the camera connected, then Refresh.")));
        outer->addStretch();
    } else {
        // Group options by sensor (Stereo Module, RGB Camera, …).
        std::map<int, QFormLayout*> formsBySensor;
        std::map<int, QString>      nameBySensor;
        for (const auto& c : controls) {
            if (!formsBySensor.count(c.sensorIndex)) {
                auto* box = new QGroupBox(QString::fromStdString(c.sensorName), fresh);
                formsBySensor[c.sensorIndex] = new QFormLayout(box);
                nameBySensor[c.sensorIndex]  = QString::fromStdString(c.sensorName);
                outer->addWidget(box);
            }
            QFormLayout* form = formsBySensor[c.sensorIndex];

            const QString tip = QString::fromStdString(c.description);
            const QString label = QString::fromStdString(c.name);
            const int   sensorIdx = c.sensorIndex;
            const int   optionId  = c.optionId;

            if (c.isBool) {
                auto* cb = new QCheckBox;
                cb->setChecked(c.current >= 0.5f);
                cb->setEnabled(!c.readOnly);
                cb->setToolTip(tip);
                connect(cb, &QCheckBox::toggled, this, [this, sensorIdx, optionId](bool on) {
                    m_camera->setControl(sensorIdx, optionId, on ? 1.0f : 0.0f);
                });
                auto* lbl = new QLabel(label);
                lbl->setToolTip(tip);
                form->addRow(lbl, cb);
            } else {
                auto* spin = new QDoubleSpinBox;
                spin->setRange(c.min, c.max);
                spin->setSingleStep(c.step > 0 ? c.step : 1.0);
                // Integer-valued option (step is whole) → no decimals.
                const bool integral = (c.step >= 1.0f) &&
                    (c.step == static_cast<float>(static_cast<long long>(c.step)));
                spin->setDecimals(integral ? 0 : 3);
                spin->setValue(c.current);
                spin->setEnabled(!c.readOnly);
                spin->setToolTip(tip);
                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, [this, sensorIdx, optionId](double v) {
                    m_camera->setControl(sensorIdx, optionId, static_cast<float>(v));
                });
                auto* lbl = new QLabel(label);
                lbl->setToolTip(tip);
                form->addRow(lbl, spin);
            }
        }
        outer->addStretch();
    }

    // setWidget() takes ownership and deletes the previously-set widget (with
    // all its child controls), so this cleanly replaces the old options.
    m_scroll->setWidget(fresh);
}

} // namespace ibom::gui
