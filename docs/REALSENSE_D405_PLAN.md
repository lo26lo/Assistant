# Plan — Intégration caméra Intel RealSense D405 (dual-backend)

> **Date** : 2026-06-15 · **Statut** : ✅ Phases 1, 2 et 2.5 **implémentées** (compile/link OK sur Jetson, à valider avec la D405 branchée) · Phase 3 = 📋 plan seulement ([REALSENSE_D405_PHASE3_PLAN.md](REALSENSE_D405_PHASE3_PLAN.md))
> **Objectif** : ajouter la **D405** comme source caméra **en parallèle** du microscope USB existant — l'utilisateur choisit le backend au runtime, les deux restent fonctionnels. Puis exploiter la **profondeur** (depth) que la webcam ne peut pas fournir.
> **Documents liés** : [JETSON_MIGRATION.md](JETSON_MIGRATION.md), [AI_PIPELINE.md](AI_PIPELINE.md), [DATASET_CREATOR_PLAN.md](DATASET_CREATOR_PLAN.md).

---

## 0. Principe directeur : DUAL-BACKEND, jamais un remplacement

L'exigence centrale : **pouvoir travailler avec les deux variantes** — la D405 **et** le microscope USB. On n'enlève rien à l'existant. On introduit une **interface commune** (`ICameraSource`) que les deux implémentations respectent, et `Application` choisit laquelle instancier selon la config / le sélecteur UI.

```
                    ┌─────────────────────────┐
                    │      ICameraSource       │  (QObject, interface)
                    │  signals: frameReady,    │
                    │   captureError,          │
                    │   captureStateChanged    │
                    │  methods: start/stop/…   │
                    └────────────┬────────────-┘
                       ▲                    ▲
          ┌────────────┴──────┐   ┌─────────┴───────────────┐
          │  CameraCapture    │   │   RealSenseCapture       │
          │  (V4L2 / UVC)     │   │   (librealsense2)        │
          │  microscope USB   │   │   D405 — color + depth   │
          └───────────────────┘   └──────────────────────────┘
```

`Application` détient un `std::unique_ptr<ICameraSource>` ; au démarrage et lors d'un changement de backend, il détruit l'ancien et instancie le bon. **Tout l'aval (CameraView, TrackingWorker, OverlayRenderer, DatasetCreator) reste inchangé** : il consomme `frameReady(FrameRef)` exactement comme aujourd'hui.

---

## 1. État actuel — ce qui est DÉJÀ fait (Docker)

Bonne surprise : l'infrastructure D405 a été anticipée dans `docker/base.Dockerfile`.

| Élément | État | Référence |
|---|:--:|---|
| librealsense2 **v2.55.1** compilée from source | ✅ | `base.Dockerfile` STAGE 2 (l.86-114) |
| Build avec **CUDA** (`BUILD_WITH_CUDA=ON`) | ✅ | l.108 |
| Backend **RSUSB** (`FORCE_RSUSB_BACKEND=ON`, pas de patch kernel) | ✅ | l.109 |
| Install dans l'image runtime (`/usr/local`) | ✅ | l.309 |
| Règles udev `99-realsense-libusb.rules` | ✅ | l.315-316 |

**Ce qui manque** (donc le périmètre de ce plan) :
- ❌ `find_package(realsense2)` + link dans `CMakeLists.txt`
- ❌ Toute la couche C++ (`ICameraSource`, `RealSenseCapture`)
- ❌ Sélecteur de backend dans `Config` + UI
- ❌ Passthrough `/dev/bus/usb` activé dans le workflow local (commenté pour l'instant)

> ⚠️ `FORCE_RSUSB_BACKEND=ON` = librealsense parle à la caméra via **libusb en userspace**. Conséquence pratique : dans Docker il faut mapper **`/dev/bus/usb`** (le bus USB), **pas** `/dev/video*`. La D405 n'apparaît d'ailleurs pas forcément comme un `/dev/video*` exploitable par OpenCV — c'est librealsense qui l'ouvre.

---

## 2. Apport de la D405 pour l'inspection PCB

La D405 est la RealSense **optimisée courte distance** (plage 7–50 cm), ce qui correspond pile à un poste d'inspection sous microscope.

| Flux | Résolution | Usage |
|---|---|---|
| **Couleur RGB** | jusqu'à 1280×720 | remplace/complète le microscope USB |
| **Profondeur** (stéréo active) | jusqu'à 1280×720 | distance au PCB en mm, hauteur des composants |
| **Global shutter** | — | zéro flou de mouvement (vs rolling shutter UVC) |

Gains spécifiques au projet :
- **Scale px/mm automatique** depuis la profondeur (plus besoin d'homographie pour l'échelle).
- **Hauteur composant** : détecter tombstoning, composant soulevé, pont de soudure.
- **Robustesse tracking** : global shutter → keypoints ORB stables même en mouvement.

---

## 3. Architecture cible (C++)

### 3.1 Nouvelle interface `src/camera/ICameraSource.h`

Abstraction QObject reprenant **exactement** le contrat actuel de `CameraCapture` (pour ne rien casser en aval) :

```cpp
namespace ibom::camera {

class ICameraSource : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~ICameraSource() override = default;

    virtual bool   start()              = 0;
    virtual void   stop()               = 0;
    virtual bool   isCapturing() const  = 0;
    virtual FrameRef latestFrame() const = 0;

    virtual void   setDeviceIndex(int)        = 0;
    virtual void   setResolution(int, int)    = 0;
    virtual void   setFps(int)                = 0;
    virtual QSize  resolution() const         = 0;

    // Capacités optionnelles (la webcam renvoie false / no-op)
    virtual bool   hasDepth() const { return false; }

signals:
    void frameReady(ibom::camera::FrameRef frame);
    void captureError(const QString& message);
    void captureStateChanged(bool capturing);
};

} // namespace ibom::camera
```

> `FrameRef` (= `shared_ptr<const cv::Mat>`) reste défini dans `CameraCapture.h` ; on pourra le déplacer dans `ICameraSource.h` ou un petit `FrameRef.h` partagé pour éviter une dépendance circulaire.

### 3.2 `CameraCapture` implémente l'interface

Changement minimal : `class CameraCapture : public ICameraSource`. Le code de `captureLoop()` (V4L2 + MJPG + GStreamer HW decode déjà ajouté) ne bouge pas. Les signatures de signaux sont déjà identiques.

### 3.3 Nouvelle classe `src/camera/RealSenseCapture.{h,cpp}`

Même interface, implémentation librealsense2. Thread dédié comme `CameraCapture`.

```cpp
// captureLoop() — Phase 1 (couleur seule)
rs2::pipeline pipe;
rs2::config cfg;
cfg.enable_stream(RS2_STREAM_COLOR, m_width, m_height, RS2_FORMAT_BGR8, m_fps);
pipe.start(cfg);
while (m_capturing.load()) {
    rs2::frameset fs = pipe.wait_for_frames();
    rs2::video_frame color = fs.get_color_frame();
    // Wrap zero-copy-ish dans un cv::Mat (clone car le buffer rs2 est recyclé)
    cv::Mat bgr(cv::Size(color.get_width(), color.get_height()),
                CV_8UC3, (void*)color.get_data(), cv::Mat::AUTO_STEP);
    auto shared = std::make_shared<const cv::Mat>(bgr.clone());
    { std::lock_guard lk(m_frameMutex); m_latestFrame = shared; }
    emit frameReady(shared);
}
```

> Le buffer `rs2::frame` est recyclé par la pipeline → un `.clone()` est nécessaire avant publication (contrairement au V4L2 où on possède la `cv::Mat`). En Phase 2 on pourra brancher l'`UnifiedAllocator` pour que ce clone atterrisse en mémoire CUDA UMA.

`listDevices()` (statique) : `rs2::context().query_devices()` → liste des numéros de série / noms ("Intel RealSense D405 <serial>").

### 3.4 Sélection du backend dans `Application`

```cpp
std::unique_ptr<camera::ICameraSource> m_camera;   // au lieu de CameraCapture

// createCamera() :
switch (m_config->cameraBackend()) {
    case CameraBackend::RealSense:
        m_camera = std::make_unique<camera::RealSenseCapture>();
        break;
    case CameraBackend::V4L2:
    default:
        m_camera = std::make_unique<camera::CameraCapture>(m_config->cameraIndex());
        break;
}
```

Tous les `connect(m_camera.get(), &CameraCapture::frameReady, …)` deviennent `&ICameraSource::frameReady`. Aucun autre changement en aval.

---

## 4. Découpage en phases

### Phase 1 — Couleur (D405 utilisable, dual-backend) — *priorité*

**But** : pouvoir choisir "D405" ou "Microscope USB" et que tout fonctionne comme avant.

1. `CMakeLists.txt` : `find_package(realsense2 CONFIG)` (optionnel via flag `IBOM_ENABLE_REALSENSE`, ON par défaut sur Linux), link `realsense2::realsense2`, define `IBOM_HAVE_REALSENSE`.
2. `src/camera/ICameraSource.h` (interface).
3. `CameraCapture` hérite de `ICameraSource`.
4. `src/camera/RealSenseCapture.{h,cpp}` — flux couleur seul.
5. `Config` : enum `CameraBackend { V4L2, RealSense }` + `camera.backend` JSON + migration (défaut `V4L2`).
6. `Application::createCamera()` : switch backend ; `m_camera` devient `ICameraSource`.
7. **UI** : `SettingsDialog` onglet Camera + `ControlPanel` → combo "Backend : Microscope USB / RealSense D405". Sur changement → stop/recreate/start.
8. **Docker** : `compose.local.yml` mapper `/dev/bus/usb` (ou override dynamique dans `run_local_gui.sh` / `run_dev_shell.sh`).
9. **Compilation conditionnelle** : si `realsense2` absent (build Windows legacy), `RealSenseCapture` non compilé, backend RealSense masqué dans l'UI. La webcam reste seule → zéro régression.

*Critère de succès* : sélectionner D405 → image couleur 1280×720 dans `CameraView`, overlay/tracking/dataset fonctionnent ; rebascule vers microscope USB OK à chaud.

### Phase 2 — Profondeur (valeur ajoutée) — ✅ IMPLÉMENTÉE (2026-06-15, à valider D405 branchée)

**But** : exposer la depth que la webcam ne peut pas donner.

1. ✅ `using DepthFrameRef = std::shared_ptr<const cv::Mat>;` (`CV_16UC1`, mm/pixel) dans `ICameraSource.h`.
2. ✅ `RealSenseCapture` : `enable_stream(RS2_STREAM_DEPTH, Z16)` + `rs2::align(RS2_STREAM_COLOR)`. Depth convertie en **mm** (via `get_depth_scale()`).
3. ✅ Signal `depthFrameReady(DepthFrameRef)` **dans `RealSenseCapture`** ; `Application::wireCameraSignals` se connecte via `dynamic_cast<RealSenseCapture*>` (rien pour la webcam).
4. ✅ **Scale px/mm auto** : `pixelsPerMm = colorFx / distance_mm` (géométrie pinhole, intrinsèques usine), distance = médiane ROI centrale 20% (0 ignorés). Actif quand `ScaleMethod::Depth`. Met à jour calibration + measurement + CameraView.
5. ✅ `StatsPanel::setDistance()` : "Distance : 23.4 mm" live (throttle 3 Hz).

### Phase 2.5 — Panneau d'options capteur (comme le RealSense Viewer) — ✅ IMPLÉMENTÉE (2026-06-15)

**But** : exposer **toutes** les options du capteur (exposition, gain, puissance laser, emitter, presets depth, white balance…) avec une **info-bulle explicative sur chacune**, exactement comme l'app Windows.

Principe : on n'écrit pas la liste en dur — librealsense la fournit. Pour chaque sensor (`device.query_sensors()`) on parcourt les `rs2_option` supportées et on lit `range` (min/max/step/def), `get_option()`, `is_option_read_only()` et surtout **`get_option_description()`** (le texte d'aide du SDK → tooltip).

1. ✅ `RealSenseCapture` publie le `rs2::device` live (mutex) ; `listControls()` → `vector<RsControl>` (descripteurs), `setControl(sensor, option, value)`.
2. ✅ `gui/RealSenseControlsDialog` génère **dynamiquement** un contrôle par option : checkbox (bool) ou spinbox (numérique), groupés par sensor, tooltip = description SDK, application immédiate, bouton Refresh.
3. ✅ `ControlPanel` : bouton « Camera Controls (RealSense)… » → `Application` ouvre le dialog (message si backend ≠ RealSense ou caméra arrêtée).
4. ✅ Compilé conditionnellement (`IBOM_HAVE_REALSENSE`).
5. ✅ **Filtres depth configurables** : le panneau expose aussi les groupes Spatial / Temporal / Threshold / Hole Filling (case « Enabled » + tous leurs paramètres rs2 avec tooltip), même mécanique d'énumération dynamique. Spatial + Temporal actifs par défaut.

#### Réglages recommandés D405 pour l'inspection PCB/SMD (~20 cm)

> Synthèse des recommandations Intel/RealSense (issue #10682 + docs), par ordre d'impact :

| Réglage | Valeur conseillée | Où |
|---|---|---|
| **Résolution / FPS** | **848×480 @ 30** (optimal précision depth) — par défaut sur RealSense désormais. 1280×720 possible si plus de détail RGB voulu (compromis : −précision depth). Même rés. depth/IR/RGB obligatoire sur D405. | défaut code / Settings |
| **Visual Preset** | **High Accuracy** (depth propre, moins de faux points) ou Medium Density si plus de remplissage. (Pas de « Short Range » sur D405 — c'était la L515.) | panneau RealSense |
| **Exposition / gain** | **exposition manuelle** (setup fixe) + **gain minimal (~16)** | panneau RealSense |
| **Filtres** | scène statique → **Temporal** (moyenne multi-frames) + **Spatial** ; décimation seulement si perf | actifs par défaut + panneau |
| **Disparity shift** (Advanced > Depth Table) | **0** à 20 cm (plage idéale 7–50 cm). N'ajuster (~20–100) que si on descend sous 7 cm — dépend de la résolution. | panneau (Advanced) |
| **Polariseur** | film **polariseur linéaire** devant les capteurs si soudures/composants brillants → annule les reflets, depth bien plus lisible (astuce équipe RealSense) | matériel |

Ordre de réglage : preset High Accuracy → exposition manuelle + gain 16 → 848×480 → temporal+spatial → polariseur si reflets → disparity shift seulement si < 7 cm.

#### Profils 1-clic (dans le panneau RealSense)

Quatre profils prêts à l'emploi (combo « Profil » + description gains/pertes + bouton Appliquer) qui règlent **résolution + FPS + Visual Preset + filtres** d'un coup :

| Profil | Rés. | Preset | Gain / Perte |
|---|---|---|---|
| **Précision depth** (recommandé) | 848×480@30 | High Accuracy | + depth précise/stable pour mesure / − RGB moins fin, quelques trous |
| **Détail visuel** | 1280×720@30 | Medium Density | + image/overlay nets / − précision depth ↓, charge USB ↑ |
| **Remplissage maximal** | 848×480@30 | High Density + hole filling | + surfaces lisses couvertes / − plus de bruit, moins fiable |
| **Aperçu rapide** | 480×270@60 | — | + fluide, latence basse / − précision/détail faibles |

Appliquer redémarre le flux (changement de résolution) et applique le preset au (re)start.

### Phase 3 — Inspection 3D (différenciateur, à planifier séparément)

- Heatmap hauteur (overlay sur `CameraView`).
- Validation hauteur Z par pad iBOM (tolérances).
- Détection tombstoning (profil de hauteur asymétrique).
- Export nuage de points PLY pour le rapport.

> Phase 3 fera l'objet d'un plan dédié une fois Phases 1-2 validées sur le Jetson.

---

## 5. Modifications fichier par fichier (Phase 1)

| Fichier | Action |
|---|---|
| `docker/base.Dockerfile` | ✅ rien (librealsense2 déjà présente) |
| `CMakeLists.txt` | `find_package(realsense2)`, option `IBOM_ENABLE_REALSENSE`, ajout sources `RealSenseCapture.{cpp,h}`, link + define conditionnels |
| `src/camera/ICameraSource.h` | **nouveau** — interface QObject |
| `src/camera/CameraCapture.h/.cpp` | hérite de `ICameraSource` (changement minimal) |
| `src/camera/RealSenseCapture.h/.cpp` | **nouveau** — flux couleur |
| `src/app/Config.h/.cpp` | enum `CameraBackend` + getter/setter + load/save `camera.backend` |
| `src/app/Application.cpp` | `m_camera` → `unique_ptr<ICameraSource>`, `createCamera()` switch, `listDevices` par backend |
| `src/gui/SettingsDialog.cpp` | combo backend dans l'onglet Camera |
| `src/gui/ControlPanel.cpp` | (optionnel) combo backend accès rapide |
| `docker/compose.local.yml` | activer `/dev/bus/usb` (commentaire déjà en place l.31) |
| `scripts/run_local_gui.sh` / `run_dev_shell.sh` | détecter une D405 (lsusb 8086:0b5b) → ajouter `/dev/bus/usb` à l'override |

---

## 6. Docker / runtime (Phase 1)

La D405 a un VID:PID `8086:0b5b`. Passthrough requis : le **bus USB**, pas un `/dev/video*`.

```yaml
# compose.local.yml (dev + runtime)
devices:
  - /dev/bus/usb:/dev/bus/usb     # D405 via libusb (RSUSB backend)
```

Alternative dynamique (préférée, cohérente avec la gestion caméra existante) : dans `run_local_gui.sh`, si `lsusb | grep -qi 8086:0b5b`, ajouter le mapping `/dev/bus/usb` à l'override `/tmp/microscope-ibom.cameras.yml`. Évite de planter le container quand la D405 est débranchée.

Vérif rapide dans le container :
```bash
rs-enumerate-devices            # doit lister la D405 + firmware
rs-enumerate-devices -c         # streams/résolutions supportés
```

---

## 7. Schéma Config (JSON)

```jsonc
"camera": {
  "backend": "v4l2",        // "v4l2" | "realsense"  (NOUVEAU, défaut v4l2)
  "index": 0,
  "width": 1920,
  "height": 1080,
  "fps": 30,
  "hw_decode": true
}
```

> Migration `Config::load()` : `backend` absent → `v4l2` (comportement actuel). La D405 force des résolutions valides (ex. 1280×720) ; `RealSenseCapture` clampe vers le mode supporté le plus proche.

---

## 8. Risques & points de vigilance

| Risque | Mitigation |
|---|---|
| librealsense ne voit pas la D405 dans Docker | `FORCE_RSUSB_BACKEND=ON` + `/dev/bus/usb` mappé + udev rules (déjà copiées). Tester `rs-enumerate-devices` d'abord. |
| Conflit USB bandwidth (color+depth en 720p) | D405 = USB3 ; vérifier le câble/port USB3. Phase 1 ne fait que la couleur. |
| `cv::Mat` sur buffer rs2 recyclé | `.clone()` obligatoire avant publication (cf. §3.3). |
| Régression build Windows legacy | `IBOM_ENABLE_REALSENSE` OFF si `realsense2` introuvable → `RealSenseCapture` non compilé, UI masquée. |
| Résolutions D405 ≠ valeurs UI libres | `RealSenseCapture` valide/clampe vers un mode supporté ; logguer le mode retenu (comme le FOURCC actuel). |
| Changement de backend à chaud | stop()+join() de l'ancien `ICameraSource` avant de détruire/recréer (pattern déjà utilisé dans `applyCameraSettings`). |

---

## 9. Plan de test (Jetson AGX Orin JP6.2)

1. `rs-enumerate-devices` dans le container → D405 détectée.
2. `bash scripts/build_jetson.sh` → build OK avec `IBOM_HAVE_REALSENSE`.
3. App → SettingsDialog → Backend = "RealSense D405" → image couleur 1280×720.
4. Charger un iBOM → overlay + tracking ORB fonctionnent sur le flux D405.
5. Rebasculer Backend = "Microscope USB" → la webcam revient sans redémarrer l'app.
6. Débrancher la D405, démarrer l'app en V4L2 → aucun crash (backend RealSense juste indisponible).
7. `ctest --output-on-failure` → toujours vert (pas de test cassé).

---

## 10. Checklist d'implémentation (Phase 1)

- [x] `CMakeLists.txt` : option + `find_package(realsense2)` + sources + link/define
- [x] `src/camera/ICameraSource.h`
- [x] `CameraCapture` hérite de `ICameraSource`
- [x] `src/camera/RealSenseCapture.{h,cpp}` (couleur)
- [x] `Config` : `CameraBackend` + JSON + migration
- [x] `Application` : `unique_ptr<ICameraSource>` + `createCamera()` switch + hot-swap
- [x] UI : combo backend (SettingsDialog)
- [x] scripts : passthrough `/dev/bus/usb` dynamique (`run_local_gui.sh`, `run_dev_shell.sh`)
- [x] Compilation conditionnelle (source RealSense compilée seulement si `realsense2` trouvée)
- [ ] **Build + test Jetson (§9)** — à faire par l'utilisateur sur la cible

---

## 11. Calibration de la D405

⚠️ **Ne pas confondre deux calibrations distinctes** :

| Calibration | Corrige | Cible imprimée ? |
|---|---|:--:|
| **Intrinsèques RGB** (distorsion objectif) | undistort image, mesure px/mm | ❌ non (usine) |
| **Profondeur / stéréo** (depth accuracy) | exactitude des mm de depth | OCC sans cible, ou cibles Intel |

### 11.1 Intrinsèques RGB — gratuites via librealsense

La D405 est calibrée en usine ; librealsense **expose les intrinsèques**, pas besoin de damier OpenCV ni de cible :

```cpp
auto p = pipe.get_active_profile().get_stream(RS2_STREAM_COLOR)
             .as<rs2::video_stream_profile>();
rs2_intrinsics in = p.get_intrinsics();   // fx, fy, ppx, ppy, model, coeffs[5]
```

→ `RealSenseCapture` exposera ces intrinsèques pour alimenter l'undistort et le scale px/mm. **Le damier OpenCV (`CameraCalibration`) reste pour le backend microscope USB uniquement.**

### 11.2 Profondeur — On-Chip Calibration (OCC), SANS cible — recommandé

Problème des cibles imprimées Intel à courte distance — FOV D405 ≈ 87°×58° :

| Distance | Champ visible (L×H) | Cibles Intel |
|---|---|:--:|
| 7 cm (mini-Z) | ~13×8 cm | ❌ trop petit |
| 10 cm | ~19×11 cm | ❌ |
| 20 cm | ~38×22 cm | A4 paysage juste, portrait non |

- **`Calibration_Target_v1.pdf` (UCAL)** + **cible « 10 mm fixed-width bars »** = pour l'outil **Intel Dynamic Calibration** (rectification/extrinsèques stéréo). Les barres **10 mm sont à taille fixe → non redimensionnables** : inutilisables de près. La cible UCAL doit remplir le cadre → idem.
- **Solution retenue : OCC** (`rs2::auto_calibrated_device::run_on_chip_calibration`) — pointer une **surface plane texturée** (PCB nu, feuille quelconque), quelques secondes, **aucune cible**. Idéal pour un rig fixe. Garder le résultat si `|health| < 0.25`.
- **Échelle absolue** (mm exacts) : **Tare calibration** — un plan à distance connue (mesurée au pied à coulisse), sans motif particulier.

### 11.3 À implémenter (Phase 2)

- [ ] `RealSenseCapture::colorIntrinsics()` → expose `rs2_intrinsics`
- [ ] Bouton "Auto-calibrer profondeur (OCC)" dans Settings → exécute `run_on_chip_calibration`, affiche le health score, écrit la table si OK
- [ ] (optionnel) Tare calibration avec saisie de la distance connue
- [ ] Ne **jamais** dépendre des cibles imprimées par défaut (documenter comme fallback expert)

---

## 12. Estimation

| Phase | Effort | Livrable |
|---|---|---|
| Phase 1 — couleur dual-backend | ~3-4 h | D405 sélectionnable, webcam intacte |
| Phase 2 — profondeur | ~3-4 h | scale auto + distance live |
| Phase 3 — inspection 3D | plan dédié | heatmap hauteur, validation Z, export PLY |
