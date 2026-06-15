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
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QTimer>
#include <QVector>

#include <map>

namespace ibom::gui {

namespace {

/// A one-click resolution/parameter profile for the D405 in PCB inspection.
struct UiProfile {
    QString name;
    QString description;     // what you gain / what you lose
    int     w, h, fps;
    QString presetLabel;     // Visual Preset to match ("" = leave as-is)
    bool    spatial, temporal, threshold, holeFill;
};

QVector<UiProfile> profiles()
{
    return {
        { QObject::tr("Précision depth (recommandé) — 848×480 @30"),
          QObject::tr("Le meilleur compromis pour l'inspection.\n"
                      "✓ Gain : depth la plus précise et stable, idéale pour mesurer des hauteurs.\n"
                      "✗ Perte : image couleur moins fine que le 720p ; quelques trous sur surfaces "
                      "difficiles (High Accuracy écarte les points peu fiables)."),
          848, 480, 30, "High Accuracy", true, true, false, false },

        { QObject::tr("Détail visuel — 1280×720 @30"),
          QObject::tr("Pour bien lire références, pistes et marquages.\n"
                      "✓ Gain : image RGB / overlay la plus nette.\n"
                      "✗ Perte : précision depth légèrement réduite vs 848×480 ; plus de charge USB/CPU."),
          1280, 720, 30, "Medium Density", true, true, false, false },

        { QObject::tr("Remplissage maximal — 848×480 @30"),
          QObject::tr("Depth la plus « pleine », peu de trous.\n"
                      "✓ Gain : surfaces lisses/brillantes mieux couvertes (hole filling).\n"
                      "✗ Perte : plus de faux points / bruit — moins fiable pour une mesure fine."),
          848, 480, 30, "High Density", true, true, false, true },

        { QObject::tr("Aperçu rapide — 480×270 @60"),
          QObject::tr("Pour cadrer / positionner la carte rapidement.\n"
                      "✓ Gain : fluidité et latence basses.\n"
                      "✗ Perte : précision et détail faibles — pas pour la mesure."),
          480, 270, 60, "", true, false, false, false },
    };
}

} // namespace

RealSenseControlsDialog::RealSenseControlsDialog(camera::RealSenseCapture* camera,
                                                 QWidget* parent)
    : QDialog(parent)
    , m_camera(camera)
{
    setWindowTitle(tr("RealSense — Camera Controls"));
    setMinimumSize(440, 560);

    // If the camera goes away (e.g. backend hot-swap destroys it), close this
    // dialog so no later callback or delayed rebuild() touches a dead object.
    if (camera)
        connect(camera, &QObject::destroyed, this, &QDialog::close);

    auto* root = new QVBoxLayout(this);

    // ── Resolution / parameter profiles ──
    auto* profBox = new QGroupBox(tr("Profil"), this);
    auto* profLay = new QVBoxLayout(profBox);
    auto* profRow = new QHBoxLayout;
    auto* profCombo = new QComboBox;
    const auto profs = profiles();
    for (const auto& p : profs) profCombo->addItem(p.name);
    auto* applyProf = new QPushButton(tr("Appliquer"));
    profRow->addWidget(profCombo, 1);
    profRow->addWidget(applyProf);
    profLay->addLayout(profRow);

    m_profileDesc = new QLabel(profs.isEmpty() ? QString() : profs[0].description);
    m_profileDesc->setWordWrap(true);
    m_profileDesc->setStyleSheet("color: palette(mid);");
    profLay->addWidget(m_profileDesc);
    root->addWidget(profBox);

    connect(profCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i) {
        const auto ps = profiles();
        if (i >= 0 && i < ps.size()) m_profileDesc->setText(ps[i].description);
    });
    connect(applyProf, &QPushButton::clicked, this, [this, profCombo]() {
        applyProfile(profCombo->currentIndex());
    });

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
                    if (m_camera) m_camera->setControl(sensorIdx, optionId, on ? 1.0f : 0.0f);
                });
                auto* lbl = new QLabel(label);
                lbl->setToolTip(tip);
                form->addRow(lbl, cb);
            } else if (!c.enumValues.empty()) {
                // Discrete enum (e.g. Visual Preset) → combo with named values.
                auto* combo = new QComboBox;
                int currentIdx = 0;
                for (int i = 0; i < static_cast<int>(c.enumValues.size()); ++i) {
                    const auto& [val, name] = c.enumValues[i];
                    combo->addItem(QString::fromStdString(name), val);
                    if (val == c.current) currentIdx = i;
                }
                combo->setCurrentIndex(currentIdx);
                combo->setEnabled(!c.readOnly);
                combo->setToolTip(tip);
                connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this, combo, sensorIdx, optionId](int) {
                    if (m_camera)
                        m_camera->setControl(sensorIdx, optionId,
                                             combo->currentData().toFloat());
                });
                auto* lbl = new QLabel(label);
                lbl->setToolTip(tip);
                form->addRow(lbl, combo);
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
                    if (m_camera) m_camera->setControl(sensorIdx, optionId, static_cast<float>(v));
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

void RealSenseControlsDialog::applyProfile(int index)
{
    const auto ps = profiles();
    if (!m_camera || index < 0 || index >= ps.size()) return;
    const UiProfile& p = ps[index];
    using RS = camera::RealSenseCapture;

    // Resolve the Visual Preset value from the live device (before restart) by
    // matching the SDK's value label, then queue it for the next start.
    if (!p.presetLabel.isEmpty()) {
        for (const auto& c : m_camera->listControls()) {
            if (!QString::fromStdString(c.name).contains("Visual Preset", Qt::CaseInsensitive))
                continue;
            for (const auto& [val, label] : c.enumValues) {
                if (QString::fromStdString(label).contains(p.presetLabel, Qt::CaseInsensitive)) {
                    m_camera->setPendingVisualPreset(val);
                    break;
                }
            }
            break;
        }
    }

    // Filter on/off (FilterChain order: Spatial, Temporal, Threshold, HoleFill).
    m_camera->setControl(RS::kFilterBase + 0, RS::kEnableOption, p.spatial   ? 1.f : 0.f);
    m_camera->setControl(RS::kFilterBase + 1, RS::kEnableOption, p.temporal  ? 1.f : 0.f);
    m_camera->setControl(RS::kFilterBase + 2, RS::kEnableOption, p.threshold ? 1.f : 0.f);
    m_camera->setControl(RS::kFilterBase + 3, RS::kEnableOption, p.holeFill  ? 1.f : 0.f);

    // Resolution/fps need a pipeline restart.
    const bool wasCapturing = m_camera->isCapturing();
    if (wasCapturing) m_camera->stop();
    m_camera->setResolution(p.w, p.h);
    m_camera->setFps(p.fps);
    if (wasCapturing) m_camera->start();

    // The device republishes asynchronously on the capture thread; refresh the
    // controls once it is back up.
    QTimer::singleShot(1200, this, &RealSenseControlsDialog::rebuild);
}

} // namespace ibom::gui
