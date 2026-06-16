#include "RealSenseControlsDialog.h"
#include "ToggleSwitch.h"
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
#include <QSlider>
#include <QSignalBlocker>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QTimer>
#include <QVector>
#include <QFileDialog>
#include <QMessageBox>
#include <QDateTime>

#include <map>
#include <cmath>
#include <algorithm>
#include <QFont>
#include <QSettings>

namespace ibom::gui {

namespace {

/// A one-click resolution/parameter profile for the D405 in PCB inspection.
struct UiProfile {
    QString name;
    QString description;     // what you gain / what you lose
    int     w, h, fps;
    QString presetLabel;     // Visual Preset to match ("" = leave as-is)
    bool    spatial, temporal, threshold, holeFill;
    int     disparityShift = -1;  // advanced-mode disparity shift (-1 = leave)
    bool    aeRoiCenter = false;  // meter auto-exposure on the central region
    int     secondPeak = -1;      // advanced-mode Second Peak Threshold (-1 = leave)
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

        { QObject::tr("Inspection rapprochée (réglé) — 848×480 @30"),
          QObject::tr("Profil « tuning » Intel pour petits composants de très près.\n"
                      "✓ Gain : zone de mesure rapprochée (disparity shift), depth fine "
                      "et stable (High Accuracy + spatial/temporal alpha 0.1 + Second "
                      "Peak Threshold 0 → fluctuation minimale sur carte fixe), "
                      "exposition mesurée sur la carte (ROI centrale).\n"
                      "✗ Perte : portée max réduite — pensé pour la distance de travail "
                      "courte de la D405. La depth devient quasi statique (idéal pour "
                      "mesurer une carte immobile). Augmente le disparity shift si la "
                      "carte est très proche et apparaît « trouée »."),
          848, 480, 30, "High Accuracy", true, true, false, false,
          /*disparityShift=*/64, /*aeRoiCenter=*/true, /*secondPeak=*/0 },
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

    // ── Streams + live health (Viewer-style stream column) ──
    auto* streamBox = new QGroupBox(tr("Flux"), this);
    auto* streamForm = new QFormLayout(streamBox);

    m_colorFpsLabel = new QLabel("—");
    streamForm->addRow(tr("Color (toujours actif):"), m_colorFpsLabel);

    auto* depthRow = new QWidget;
    auto* depthHl  = new QHBoxLayout(depthRow);
    depthHl->setContentsMargins(0, 0, 0, 0);
    auto* depthSw = new ToggleSwitch;
    depthSw->setChecked(m_camera ? m_camera->depthStreamEnabled() : true);
    depthSw->setOffset(depthSw->isChecked() ? 1.0f : 0.0f);
    depthSw->setToolTip(tr("Active/désactive le flux depth (3D, vue depth, scale). "
                           "Prend effet en redémarrant la caméra."));
    m_depthFpsLabel = new QLabel("—");
    depthHl->addWidget(depthSw);
    depthHl->addSpacing(8);
    depthHl->addWidget(m_depthFpsLabel, 1);
    streamForm->addRow(tr("Depth:"), depthRow);

    connect(depthSw, &ToggleSwitch::toggled, this, [this](bool on) {
        if (!m_camera) return;
        m_camera->setDepthStreamEnabled(on);
        // Restart so the new stream config takes effect.
        const bool wasCapturing = m_camera->isCapturing();
        if (wasCapturing) { m_camera->stop(); m_camera->start(); }
        QTimer::singleShot(900, this, &RealSenseControlsDialog::rebuild);
    });

    // Left IR camera — Intel tuning guide: use IR for reflective PCB surfaces
    // (solder joints, bare metal pads). Y8 grayscale, no color saturation.
    auto* irRow = new QWidget;
    auto* irHl  = new QHBoxLayout(irRow);
    irHl->setContentsMargins(0, 0, 0, 0);
    auto* irSw = new ToggleSwitch;
    irSw->setChecked(m_camera && m_camera->emitInfrared());
    irSw->setOffset(irSw->isChecked() ? 1.0f : 0.0f);
    irSw->setToolTip(tr("Affiche la caméra IR gauche (niveaux de gris) à la place de\n"
                        "la couleur — évite la saturation sur le métal/soudure brillant.\n"
                        "Nécessite le flux Depth actif."));
    auto* irStateLabel = new QLabel(m_camera && m_camera->emitInfrared() ? tr("actif") : "—");
    irHl->addWidget(irSw);
    irHl->addSpacing(8);
    irHl->addWidget(irStateLabel, 1);
    streamForm->addRow(tr("IR gauche (niveaux de gris):"), irRow);

    connect(irSw, &ToggleSwitch::toggled, this, [this, irStateLabel](bool on) {
        if (!m_camera) return;
        m_camera->setEmitInfrared(on);
        irStateLabel->setText(on ? tr("actif") : "—");
    });

    root->addWidget(streamBox);

    // Poll per-stream FPS ~1 Hz for the live health readout.
    auto* fpsTimer = new QTimer(this);
    fpsTimer->setInterval(1000);
    connect(fpsTimer, &QTimer::timeout, this, [this]() {
        if (!m_camera) return;
        const double cf = m_camera->colorFps();
        const double df = m_camera->depthFps();
        if (m_colorFpsLabel)
            m_colorFpsLabel->setText(cf > 0 ? QString("%1 fps").arg(cf, 0, 'f', 1) : "—");
        if (m_depthFpsLabel)
            m_depthFpsLabel->setText(df > 0 ? QString("%1 fps").arg(df, 0, 'f', 1) : "—");
    });
    fpsTimer->start();

    // ── Tools (Viewer-style): AE ROI, PLY export, on-chip self-cal ──
    auto* toolsBox = new QGroupBox(tr("Outils"), this);
    auto* toolsLay = new QVBoxLayout(toolsBox);

    // Disparity shift (advanced mode) — close-range tuning per Intel's guide.
    auto* dispRow = new QHBoxLayout;
    dispRow->addWidget(new QLabel(tr("Disparity shift:")));
    auto* dispSlider = new QSlider(Qt::Horizontal);
    dispSlider->setRange(0, 256);
    auto* dispVal = new QLabel("—");
    dispVal->setFixedWidth(36);
    dispSlider->setToolTip(tr("Décale la fenêtre de mesure depth vers l'avant : "
                              "augmente pour mesurer de très près (petits composants). "
                              "Nécessite le mode avancé activé sur la caméra."));
    if (m_camera) {
        const int cur = m_camera->disparityShift();
        if (cur >= 0) { dispSlider->setValue(cur); dispVal->setText(QString::number(cur)); }
        else { dispSlider->setEnabled(false); dispVal->setText(tr("N/A")); }
    }
    connect(dispSlider, &QSlider::valueChanged, this, [this, dispVal](int v) {
        dispVal->setText(QString::number(v));
        if (m_camera) m_camera->setDisparityShift(v);
    });
    dispRow->addWidget(dispSlider, 1);
    dispRow->addWidget(dispVal);
    toolsLay->addLayout(dispRow);

    // Second Peak Threshold (advanced mode) — lower = less depth fluctuation on
    // a static scene (Intel/MartyG, issue #10682). Default 325; 0 = most stable.
    auto* peakRow = new QHBoxLayout;
    peakRow->addWidget(new QLabel(tr("Second Peak Threshold:")));
    auto* peakSlider = new QSlider(Qt::Horizontal);
    peakSlider->setRange(0, 1023);
    auto* peakVal = new QLabel("—");
    peakVal->setFixedWidth(36);
    peakSlider->setToolTip(tr("Réduit la fluctuation de la depth sur une scène fixe : "
                              "baisse vers 0 (défaut 325) pour une carte immobile, "
                              "mesure plus stable. La depth réagit alors plus lentement "
                              "au mouvement. Nécessite le mode avancé activé."));
    if (m_camera) {
        const int cur = m_camera->secondPeakThreshold();
        if (cur >= 0) { peakSlider->setValue(cur); peakVal->setText(QString::number(cur)); }
        else { peakSlider->setEnabled(false); peakVal->setText(tr("N/A")); }
    }
    connect(peakSlider, &QSlider::valueChanged, this, [this, peakVal](int v) {
        peakVal->setText(QString::number(v));
        if (m_camera) m_camera->setSecondPeakThreshold(v);
    });
    peakRow->addWidget(peakSlider, 1);
    peakRow->addWidget(peakVal);
    toolsLay->addLayout(peakRow);

    auto* aeBtn = new QPushButton(tr("Auto-exposition sur le centre"));
    aeBtn->setToolTip(tr("Règle l'auto-exposition en mesurant la zone centrale "
                         "(la carte) plutôt que tout le champ — image plus stable."));
    connect(aeBtn, &QPushButton::clicked, this, [this]() {
        if (!m_camera) return;
        const bool ok = m_camera->setAutoExposureRoi(0, 0, 0, 0);  // central 50%
        QMessageBox::information(this, tr("Auto-exposition"),
            ok ? tr("ROI d'auto-exposition réglée sur le centre.")
               : tr("Ce capteur ne supporte pas la ROI d'auto-exposition."));
    });
    toolsLay->addWidget(aeBtn);

    // Cloud/PLY decimation — Intel's canonical 3D-scan order (issue #10682).
    // Affects ONLY the 3D view + PLY export, never the overlay/depth-view depth.
    auto* decRow = new QHBoxLayout;
    decRow->addWidget(new QLabel(tr("Décimation nuage 3D:")));
    auto* decCombo = new QComboBox;
    decCombo->setToolTip(tr("Sous-échantillonne la profondeur du nuage 3D / export PLY :\n"
                            "scan plus propre et moins bruité, moins de points.\n"
                            "N'affecte PAS la vue depth ni l'alignement de l'overlay iBOM."));
    decCombo->addItem(tr("Désactivée (pleine résolution)"), 0);
    decCombo->addItem(QStringLiteral("×2"), 2);
    decCombo->addItem(QStringLiteral("×3"), 3);
    decCombo->addItem(QStringLiteral("×4"), 4);
    if (m_camera) {
        const int cur = m_camera->cloudDecimation();
        const int idx = decCombo->findData(cur >= 2 ? cur : 0);
        decCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    connect(decCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, decCombo](int) {
        if (m_camera) m_camera->setCloudDecimation(decCombo->currentData().toInt());
    });
    decRow->addWidget(decCombo, 1);
    toolsLay->addLayout(decRow);

    auto* plyBtn = new QPushButton(tr("Exporter le nuage 3D (PLY)…"));
    plyBtn->setToolTip(tr("Enregistre le prochain nuage de points (sommets + "
                          "couleur) dans un fichier .ply."));
    connect(plyBtn, &QPushButton::clicked, this, [this]() {
        if (!m_camera) return;
        const QString def = QString("pointcloud_%1.ply")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Exporter le nuage de points"), def, tr("PLY (*.ply)"));
        if (!path.isEmpty()) m_camera->requestPlyExport(path.toStdString());
    });
    toolsLay->addWidget(plyBtn);

    auto* calBtn = new QPushButton(tr("Self-calibration depth (sans mire)…"));
    calBtn->setToolTip(tr("Lance la calibration depth embarquée du D4xx. "
                          "Bloque quelques secondes ; expérimental."));
    connect(calBtn, &QPushButton::clicked, this, [this]() {
        if (!m_camera) return;
        if (QMessageBox::question(this, tr("Self-calibration"),
                tr("Lancer la calibration depth embarquée ?\n"
                   "Garde la caméra immobile face à une surface plane texturée "
                   "pendant quelques secondes.")) == QMessageBox::Yes) {
            m_camera->requestOnChipCalibration();
        }
    });
    toolsLay->addWidget(calBtn);

    // Advanced-mode JSON presets (load/save full device config).
    auto* presetRow = new QHBoxLayout;
    auto* loadJson = new QPushButton(tr("Charger preset JSON…"));
    loadJson->setToolTip(tr("Applique une configuration complète (advanced mode) "
                            "depuis un fichier .json — presets recommandés Intel."));
    connect(loadJson, &QPushButton::clicked, this, [this]() {
        if (!m_camera) return;
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Charger un preset"), QString(), tr("JSON (*.json)"));
        if (path.isEmpty()) return;
        const bool ok = m_camera->loadJsonPreset(path.toStdString());
        QMessageBox::information(this, tr("Preset"),
            ok ? tr("Preset appliqué.") : tr("Échec du chargement du preset."));
        if (ok) rebuild();  // reflect the new option values
    });
    auto* saveJson = new QPushButton(tr("Enregistrer preset JSON…"));
    saveJson->setToolTip(tr("Sauve la configuration courante du capteur en .json."));
    connect(saveJson, &QPushButton::clicked, this, [this]() {
        if (!m_camera) return;
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Enregistrer le preset"), "realsense_preset.json",
            tr("JSON (*.json)"));
        if (path.isEmpty()) return;
        const bool ok = m_camera->saveJsonPreset(path.toStdString());
        QMessageBox::information(this, tr("Preset"),
            ok ? tr("Preset enregistré.") : tr("Échec de l'enregistrement."));
    });
    presetRow->addWidget(loadJson);
    presetRow->addWidget(saveJson);
    toolsLay->addLayout(presetRow);

    // Rosbag recording (takes effect on next camera restart).
    auto* recBtn = new QPushButton(tr("Enregistrer en .bag (au prochain start)…"));
    recBtn->setCheckable(true);
    recBtn->setToolTip(tr("Enregistre tous les flux dans un rosbag .bag, comme le "
                          "bouton Record du Viewer. Prend effet au prochain "
                          "démarrage caméra. Recliquer pour désactiver."));
    connect(recBtn, &QPushButton::toggled, this, [this, recBtn](bool on) {
        if (!m_camera) { recBtn->setChecked(false); return; }
        if (on) {
            const QString def = QString("capture_%1.bag")
                .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
            const QString path = QFileDialog::getSaveFileName(
                this, tr("Fichier d'enregistrement"), def, tr("Rosbag (*.bag)"));
            if (path.isEmpty()) { recBtn->setChecked(false); return; }
            m_camera->setRecordFile(path.toStdString());
            recBtn->setText(tr("Enregistrement armé — redémarre la caméra"));
        } else {
            m_camera->setRecordFile("");
            recBtn->setText(tr("Enregistrer en .bag (au prochain start)…"));
        }
    });
    toolsLay->addWidget(recBtn);

    root->addWidget(toolsBox);

    // Surface async results from the capture thread.
    if (m_camera) {
        connect(m_camera, &camera::RealSenseCapture::plyExportFinished, this,
                [this](bool ok, const QString& msg) {
            QMessageBox::information(this, tr("Export PLY"),
                ok ? tr("Nuage exporté :\n%1").arg(msg)
                   : tr("Échec de l'export :\n%1").arg(msg));
        });
        connect(m_camera, &camera::RealSenseCapture::onChipCalibrationFinished, this,
                [this](bool ok, float health, const QString& msg) {
            Q_UNUSED(health);
            QMessageBox::information(this, tr("Self-calibration"), msg);
        });
    }

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

QWidget* RealSenseControlsDialog::buildControlRow(const camera::RsControl& c)
{
    const QString tip = QString::fromStdString(c.description);
    const int sensorIdx = c.sensorIndex;
    const int optionId  = c.optionId;

    if (c.isBool) {
        auto* sw = new ToggleSwitch;
        sw->setChecked(c.current >= 0.5f);
        sw->setOffset(c.current >= 0.5f ? 1.0f : 0.0f);
        sw->setEnabled(!c.readOnly);
        sw->setToolTip(tip);
        connect(sw, &ToggleSwitch::toggled, this, [this, sensorIdx, optionId](bool on) {
            if (m_camera) m_camera->setControl(sensorIdx, optionId, on ? 1.0f : 0.0f);
        });
        // Left-align the switch so it doesn't stretch across the form field.
        auto* wrap = new QWidget;
        auto* hl = new QHBoxLayout(wrap);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->addWidget(sw);
        hl->addStretch();
        return wrap;
    }

    if (!c.enumValues.empty()) {
        // Discrete enum (e.g. Visual Preset) → combo with the SDK's named values.
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
                m_camera->setControl(sensorIdx, optionId, combo->currentData().toFloat());
        });
        return combo;
    }

    // Numeric option → slider + value box, like the RealSense Viewer.
    const double step  = c.step > 0 ? c.step : 1.0;
    const int    steps = std::max(1, static_cast<int>(std::lround((c.max - c.min) / step)));
    const bool   integral = (c.step >= 1.0f) &&
        (c.step == static_cast<float>(static_cast<long long>(c.step)));

    auto* row    = new QWidget;
    auto* h      = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* slider = new QSlider(Qt::Horizontal);
    auto* spin   = new QDoubleSpinBox;
    slider->setRange(0, steps);
    slider->setValue(static_cast<int>(std::lround((c.current - c.min) / step)));
    slider->setEnabled(!c.readOnly);
    slider->setToolTip(tip);
    spin->setRange(c.min, c.max);
    spin->setSingleStep(step);
    spin->setDecimals(integral ? 0 : 3);
    spin->setValue(c.current);
    spin->setEnabled(!c.readOnly);
    spin->setToolTip(tip);
    spin->setFixedWidth(84);
    h->addWidget(slider, 1);
    h->addWidget(spin);

    // Keep slider ↔ spin in sync without feedback loops, and push to the device.
    connect(slider, &QSlider::valueChanged, this,
            [this, spin, sensorIdx, optionId, minv = c.min, step](int pos) {
        const double v = minv + pos * step;
        { QSignalBlocker b(spin); spin->setValue(v); }
        if (m_camera) m_camera->setControl(sensorIdx, optionId, static_cast<float>(v));
    });
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this, slider, sensorIdx, optionId, minv = c.min, step](double v) {
        { QSignalBlocker b(slider); slider->setValue(static_cast<int>(std::lround((v - minv) / step))); }
        if (m_camera) m_camera->setControl(sensorIdx, optionId, static_cast<float>(v));
    });
    return row;
}

QGroupBox* RealSenseControlsDialog::makeCollapsibleGroup(const QString& title,
                                                         QFormLayout*& formOut)
{
    // Checkable group box that collapses its content (Viewer-style panels).
    // The expanded/collapsed state persists per group title via QSettings.
    QSettings settings("PCBInspector", "RealSenseControls");
    const QString key = "group/" + title;
    const bool expanded = settings.value(key, true).toBool();

    auto* box = new QGroupBox(title);
    box->setCheckable(true);
    box->setChecked(expanded);
    auto* inner = new QWidget;
    inner->setVisible(expanded);
    formOut = new QFormLayout(inner);
    formOut->setLabelAlignment(Qt::AlignLeft);
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(6, 4, 6, 4);
    lay->addWidget(inner);
    connect(box, &QGroupBox::toggled, inner, [this, inner, key](bool on) {
        inner->setVisible(on);
        QSettings("PCBInspector", "RealSenseControls").setValue(key, on);
    });
    return box;
}

namespace {
// Curated display priority so the most-used options float to the top of each
// group, roughly matching the RealSense Viewer's ordering. Lower = higher.
int optionPriority(const std::string& name)
{
    static const std::vector<std::string> order = {
        "Visual Preset",
        "Enable Auto Exposure", "Exposure", "Gain",
        "Enable Auto White Balance", "White Balance",
        "Laser Power", "Emitter Enabled", "Emitter On Off",
        "Brightness", "Contrast", "Gamma", "Hue", "Saturation", "Sharpness",
        "Backlight Compensation", "Power Line Frequency",
        "Enabled",  // filter on/off toggles first within a filter group
    };
    for (size_t i = 0; i < order.size(); ++i)
        if (name.find(order[i]) != std::string::npos)
            return static_cast<int>(i);
    return 1000;  // everything else, kept in enum order after the curated ones
}
}

void RealSenseControlsDialog::rebuild()
{
    using RS = camera::RealSenseCapture;

    // Replace the content widget wholesale — simplest way to clear old controls.
    auto* fresh = new QWidget;
    auto* outer = new QVBoxLayout(fresh);

    auto controls = m_camera ? m_camera->listControls()
                             : std::vector<camera::RsControl>{};
    // Stable sort within each owner (sensor/filter) by curated priority, so the
    // section/group order is preserved but options are Viewer-ordered.
    std::stable_sort(controls.begin(), controls.end(),
        [](const camera::RsControl& a, const camera::RsControl& b) {
            if (a.sensorIndex != b.sensorIndex) return a.sensorIndex < b.sensorIndex;
            return optionPriority(a.name) < optionPriority(b.name);
        });

    if (controls.empty()) {
        outer->addWidget(new QLabel(
            tr("No RealSense options available.\n"
               "Start the RealSense backend with the camera connected, then Refresh.")));
        outer->addStretch();
        m_scroll->setWidget(fresh);
        return;
    }

    // Two sections like the Viewer: sensor "Controls" and "Post-Processing"
    // (filters live at ownerId >= kFilterBase).
    auto addSectionHeader = [&](const QString& text) {
        auto* h = new QLabel(text);
        QFont f = h->font(); f.setBold(true); f.setPointSizeF(f.pointSizeF() + 1);
        h->setFont(f);
        h->setStyleSheet("color: palette(highlight); margin-top: 6px;");
        outer->addWidget(h);
    };

    std::map<int, QFormLayout*> formBySensor;  // sensor groups (controls)
    std::map<int, QFormLayout*> formByFilter;  // filter groups (post-processing)
    bool addedControlsHeader = false, addedPpHeader = false;

    for (const auto& c : controls) {
        const bool isFilter = (c.sensorIndex >= RS::kFilterBase);
        auto& forms = isFilter ? formByFilter : formBySensor;

        if (!forms.count(c.sensorIndex)) {
            if (isFilter && !addedPpHeader) {
                addSectionHeader(tr("Post-Processing"));
                addedPpHeader = true;
            } else if (!isFilter && !addedControlsHeader) {
                addSectionHeader(tr("Controls"));
                addedControlsHeader = true;
            }
            QFormLayout* form = nullptr;
            outer->addWidget(makeCollapsibleGroup(
                QString::fromStdString(c.sensorName), form));
            forms[c.sensorIndex] = form;
        }

        // Label cell: option name + a small "ⓘ" info icon carrying the SDK
        // description (hover for help), like the Viewer.
        auto* labelCell = new QWidget;
        auto* lh = new QHBoxLayout(labelCell);
        lh->setContentsMargins(0, 0, 0, 0);
        lh->setSpacing(4);
        auto* name = new QLabel(QString::fromStdString(c.name));
        name->setToolTip(QString::fromStdString(c.description));
        lh->addWidget(name);
        if (!c.description.empty()) {
            auto* info = new QLabel(QStringLiteral("ⓘ"));
            info->setToolTip(QString::fromStdString(c.description));
            info->setCursor(Qt::WhatsThisCursor);
            info->setStyleSheet("color: palette(mid);");
            lh->addWidget(info);
        }
        lh->addStretch();
        forms[c.sensorIndex]->addRow(labelCell, buildControlRow(c));
    }

    outer->addStretch();
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

    // Tuning (Intel "tuning depth cameras" / issue #10682) applied once the
    // device is back: disparity shift (closer Z window) + AE ROI on the central
    // board region + Second Peak Threshold (lower = less depth fluctuation on a
    // static scene). All advanced-mode writes are guarded internally.
    const int   shift = p.disparityShift;
    const bool  aeRoi = p.aeRoiCenter;
    const int   peak  = p.secondPeak;
    QTimer::singleShot(1200, this, [this, shift, aeRoi, peak]() {
        if (!m_camera) return;
        if (shift >= 0) m_camera->setDisparityShift(shift);
        if (aeRoi)      m_camera->setAutoExposureRoi(0, 0, 0, 0);  // central 50%
        if (peak  >= 0) m_camera->setSecondPeakThreshold(peak);
        rebuild();
    });
}

} // namespace ibom::gui
