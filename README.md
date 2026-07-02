# MicroscopeIBOM

Outil d'inspection PCB : caméra (microscope USB **ou** Intel RealSense D405) + overlay [InteractiveHtmlBom](https://github.com/openscopeproject/InteractiveHtmlBom) + inférence IA (ONNX Runtime / TensorRT).

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus)
![Qt6](https://img.shields.io/badge/Qt-6.8.2-green?logo=qt)
![OpenCV](https://img.shields.io/badge/OpenCV-4.12-orange?logo=opencv)
![ONNX Runtime](https://img.shields.io/badge/ONNX_Runtime-1.23.2-purple)
![TensorRT](https://img.shields.io/badge/TensorRT-10.15-76B900?logo=nvidia)
![Platform](https://img.shields.io/badge/Platform-Jetson%20AGX%20Orin-76B900?logo=nvidia)
![License](https://img.shields.io/badge/License-MIT-yellow)

---

## ⚠️ Plateforme

Le projet est en cours de **portage sur Jetson AGX Orin 32GB** (JetPack 6.2 / L4T R36.4) dans Docker — c'est la cible **active**.

| Branche | Plateforme | État |
|---|---|---|
| `main` / branches de dev | **Jetson AGX Orin (Linux + Docker)** | 🟢 active |
| `windows-legacy` (tag `v0.1.0-windows-final`) | Windows 11 + MSVC + vcpkg | 🧊 figée |

La version Windows reste fonctionnelle mais n'évolue plus. La doc de migration vit dans [`docs/`](docs/) :
- [docs/JETSON_MIGRATION.md](docs/JETSON_MIGRATION.md) — plan global
- [docs/JETSON_SESSION_LOG.md](docs/JETSON_SESSION_LOG.md) — état actuel + journal de sessions
- [docs/JETSON_ERREURS.md](docs/JETSON_ERREURS.md) — bugs rencontrés
- [docker/README.md](docker/README.md) — workflow Docker détaillé

---

## Fonctionnalités

### Opérationnelles

| Module | Description |
|---|---|
| **Caméra USB** | Capture temps réel 1920×1080@30fps (V4L2 / MSMF), sélecteur de périphérique, fallback pipeline MJPG GStreamer CPU |
| **Caméra RealSense D405** | Flux couleur + profondeur, self-calibration on-chip, contrôles dédiés, nuage de points 3D |
| **Calibration** | Checkerboard configurable, undistortion temps réel, PDF patron intégré, moniteur de qualité (RMS) |
| **iBOM Parser** | Import `.html` InteractiveHtmlBom — JSON direct et LZ-String compressé, reconstruction des bbox tournées |
| **Overlay PCB** | Pads, silkscreen, labels ref superposés sur le flux caméra via homographie |
| **Alignement** | 4 chemins : 4 clics (coins PCB) · 2 composants de référence · ancrage microscope 1-point · **Auto-Align** (détection automatique du contour) |
| **Auto-Align** | Détection automatique de la carte (plan de profondeur D405 ou contour 2D), désambiguïsation d'orientation, sans clic — voir [docs/AUTO_ALIGN_PLAN.md](docs/AUTO_ALIGN_PLAN.md) |
| **Live Tracking** | Optical flow LK à cadence caméra (défaut) + re-seed ORB/MAGSAC périodique, thread dédié, masque carte auto-récupérant, gates anti-saut/sanité, lissage 1€ — voir [docs/LIVE_TRACKING_ANALYSE_2026-07.md](docs/LIVE_TRACKING_ANALYSE_2026-07.md) |
| **Scale dynamique** | px/mm mis à jour automatiquement (homographie, pads iBOM, ou profondeur) |
| **Bagues optiques** | Multiplicateur 0.5×–2× appliqué au scale px/mm |
| **BOM Panel** | Tableau composants avec sélection → highlight sur l'overlay, filtres, checkboxes |
| **Minimap** | Aperçu de la carte avec position du champ de vision (`BoardMinimap`) |
| **Dataset Creator** | Capture + auto-annotation YOLO (gates qualité, mapping classes), sortie compatible Studio — voir [docs/DATASET_CREATOR_PLAN.md](docs/DATASET_CREATOR_PLAN.md) |
| **Heatmap** | Carte thermique des défauts (toggle) |
| **Settings** | Dialog 4 onglets (Camera / Overlay / Tracking / AI), persistance JSON |
| **Thème** | Dark / Light (Catppuccin Mocha / Latte), couleurs centralisées dans `Theme.h` |
| **Fullscreen** | Double-clic → plein écran caméra, Escape pour revenir |
| **Inspection** | Wizard guidé + panneau d'inspection |
| **Help** | Sections : Démarrage, Calibration, Alignement, Lentilles, Inspection, Overlay, Export, Dépannage |

### En attente de modèles / non instancié

| Module | État |
|---|---|
| **IA Détection** | 🟡 Câblé — init auto en arrière-plan si `.onnx` présent dans `models/` (YOLOv8/RT-DETR via ONNX Runtime + TensorRT) |
| **OCR** | Code prêt, non câblé |
| **Solder Inspector** | Code prêt, non câblé |
| **Pick & Place** | Code prêt, instancié partiellement |
| **Mesure** | Distance / angle / aire en mm — instancié partiellement |
| **Barcode / QR** | Code prêt, non instancié |
| **Voice Control** | Commandes vocales mains-libres — code prêt, non instancié |
| **Remote View** | Streaming WebSocket — code prêt, non instancié |
| **Stencil Align** | Code prêt, non instancié |
| **Export** | PDF / HTML / CSV / JSON — code prêt, non instancié |
| **Snapshots** | Historique annoté — instancié partiellement |

---

## Architecture

```
main.cpp
  └─ QApplication
  └─ Application (QObject)
       ├─ Config              JSON ($IBOM_DATA_DIR ou %APPDATA%)
       ├─ CameraCapture       thread → frameReady(FrameRef, captureNs) → CameraView (V4L2/MSMF)
       ├─ RealSenseCapture    thread → frameReady + depthFrameReady         (D405)
       ├─ CameraCalibration   YAML, undistort
       ├─ IBomParser          HTML → JSON → IBomProject
       ├─ OverlayRenderer     rendu espace carte (1×, AA) → warp projectif par CameraView
       ├─ Homography          pcbToImage, transformRect
       ├─ BoardLocator        Auto-Align : détection contour + désambiguïsation (Qt-free)
       ├─ HeatmapRenderer
       ├─ TrackingWorker      thread dédié — flow LK + ORB/MAGSAC, masque auto-récupérant
       ├─ DatasetCreator      thread dédié — capture + auto-annotation YOLO
       └─ MainWindow
            ├─ CameraView      paintEvent, zoom/pan, fullscreen
            ├─ PointCloudView  rendu 3D du nuage de points (D405)
            ├─ ControlPanel    toggles, sliders, bouton Auto-Align
            ├─ BomPanel        QTableWidget, filtres, checkboxes
            ├─ BoardMinimap    aperçu carte + champ de vision
            ├─ StatsPanel      FPS, GPU, scale px/mm, depth
            ├─ DatasetPanel    start/stop session, gates live, compteurs
            ├─ InspectionPanel + InspectionWizard
            ├─ SettingsDialog / RealSenseControlsDialog / CalibrationMonitorDialog / FovMeasureDialog
            └─ HelpDialog
```

### Modèle de threading

| Thread | Rôle |
|---|---|
| Main / GUI | Qt event loop, paintEvent, signals/slots UI |
| CameraCapture / RealSenseCapture | `captureLoop()` — lit le flux, émet `frameReady(FrameRef, qint64 captureNs)` (+ `depthFrameReady` pour la D405) |
| TrackingWorker | flow LK + ORB/MAGSAC — reçoit `processFrame(FrameRef, captureNs)` (backpressure max 2 en vol), émet `homographyUpdated(cv::Mat, inliers, reprojErr)` |
| DatasetCreator | Capture dataset — gates qualité, projection bboxes iBOM → labels YOLO |

`FrameRef = std::shared_ptr<const cv::Mat>` — zero-copy entre les threads.

---

## Stack technique

| Composant | Technologie | Version |
|---|---|---|
| Langage | C++20 | gcc-13 (Jetson) / MSVC 14.44 (Windows legacy) |
| GUI | Qt6 Widgets | 6.8.2 |
| Vision | OpenCV | 4.12 (Windows vcpkg) / 4.10 (Jetson, from source) |
| Caméra profondeur | librealsense2 | D405 |
| Inférence | ONNX Runtime (CUDA EP + TensorRT EP) | 1.23.2 |
| GPU | NVIDIA CUDA / cuDNN / TensorRT | selon plateforme |
| Build | CMake | ≥ 3.22 (Jetson) / 4.2.3 NMake (Windows) |
| Conteneur | Docker (JetPack 6.2 / L4T R36.4) | — |
| Logging | spdlog | 1.x |
| JSON | nlohmann/json | 3.x |
| Tests | Catch2 | 3.5+ |

---

## Build & lancement — Jetson AGX Orin (cible active)

Tout passe par Docker. Voir [docker/README.md](docker/README.md) pour le détail complet.

### Setup en une commande (Jetson vierge, JetPack 6.2)

```bash
curl -fsSL https://raw.githubusercontent.com/lo26lo/Assistant/main/scripts/bootstrap_jetson.sh | bash
```

Le script [`scripts/bootstrap_jetson.sh`](scripts/bootstrap_jetson.sh) installe Docker + nvidia-container-toolkit, clone le repo, configure les règles udev RealSense, et build les images (`microscope-ibom:base` puis `:dev`, ~2h cumulé). Idempotent.

### Workflow développement

```bash
cd ~/Assistant-git

# Shell dev avec caméra + GPU + X11 (cas général)
bash scripts/run_dev_shell.sh

# Dans le container :
bash scripts/build_jetson.sh          # Release  → build/bin/MicroscopeIBOM
bash scripts/build_jetson.sh debug    # Debug + ASAN
./build/bin/MicroscopeIBOM
```

Pour lancer directement l'app (sans shell) : `bash scripts/run_local_gui.sh`.

> ⚠️ Sur Jetson JP6.2, tout `docker run`/`docker build` doit forcer `--network host`
> (kernel Tegra sans module `iptable_raw`) — déjà appliqué dans `docker/compose.yml`.

---

## Build & lancement — Windows (legacy, branche `windows-legacy`)

```bat
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set TENSORRT_HOME=C:\TensorRT\TensorRT-10.15.1.29
set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2

cmake -B build -S . -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE=C:/Tools/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DIBOM_ENABLE_TENSORRT=ON ^
  -DQt6_DIR=C:/Qt/6.8.2/msvc2022_64/lib/cmake/Qt6

cd build && nmake
.\bin\MicroscopeIBOM.exe
```

> **NE PAS utiliser Ninja** (bug `rules.ninja` avec CMake 4.2.3) — `NMake Makefiles` uniquement.
> **NE PAS supprimer** `build\vcpkg_installed\` — onnxruntime CUDA+TRT prend ~2h à recompiler.

---

## Workflow typique

1. **Start Camera** — sélectionner le périphérique (microscope USB ou RealSense D405)
2. Si bague optique : **Settings → Camera → Lens Adapter** (0.5×–2×)
3. Calibration : **Camera → Open Calibration Patterns PDF** → imprimer et calibrer (≥ 5 captures), suivre la qualité dans le moniteur
4. **File → Open iBOM** → charger un `.html` InteractiveHtmlBom
5. **Aligner** : cliquer **Auto-Align (Beta)** pour un alignement automatique, ou aligner manuellement (4 coins / 2 composants / ancrage 1-point)
6. Activer **Live Tracking** pour que l'overlay suive les déplacements de la carte
7. Le scale px/mm se met à jour automatiquement en zoomant

---

## Raccourcis clavier

| Raccourci | Action |
|---|---|
| `Ctrl+O` | Ouvrir un fichier iBOM |
| `Ctrl+,` | Ouvrir les Settings |
| `Ctrl+S` | Sauvegarder une capture |
| `F1` | Help |
| `C` | Démarrer / arrêter la caméra |
| `Double-clic` | Plein écran caméra |
| `Escape` | Annuler / quitter le plein écran |

---

## Structure du projet

```
Assistant/
├── CMakeLists.txt
├── vcpkg.json
├── cmake/
├── docker/             base.Dockerfile, compose.yml, README.md
├── scripts/            bootstrap_jetson.sh, build_jetson.sh, run_dev_shell.sh, run_local_gui.sh
├── src/
│   ├── main.cpp
│   ├── app/            Application, Config
│   ├── camera/         CameraCapture, RealSenseCapture, ICameraSource, CameraCalibration,
│   │                   UnifiedAllocator, PointCloudData
│   ├── ibom/           IBomParser, IBomData, ComponentMap
│   ├── overlay/        Homography, OverlayRenderer, ComponentOverlay, HeatmapRenderer,
│   │                   TrackingWorker, BoardLocator
│   ├── gui/            MainWindow, CameraView, PointCloudView, BomPanel, ControlPanel,
│   │                   BoardMinimap, StatsPanel, DatasetPanel, InspectionPanel/Wizard,
│   │                   SettingsDialog, RealSenseControlsDialog, CalibrationMonitorDialog,
│   │                   FovMeasureDialog, HelpDialog, ViewModeBar, ToggleSwitch, Theme
│   ├── ai/             InferenceEngine, ModelManager, ComponentDetector, OCREngine, SolderInspector
│   ├── features/       PickAndPlace, Measurement, DatasetCreator, VoiceControl, BarcodeScanner,
│   │                   RemoteView, StencilAlign, SnapshotHistory
│   ├── export/         ReportGenerator, DataExporter
│   └── utils/          Logger, QtLogSink, GpuUtils, ImageUtils, Paths
├── tests/              Catch2 — ibom_parser, homography, inference, component_matching
├── tools/dataset_studio/   annotation / dataset studio (Python)
├── models/             Modèles ONNX (voir models/README.md)
├── resources/
└── docs/               JETSON_*, AUTO_ALIGN_PLAN, DATASET_CREATOR_PLAN, AI_PIPELINE…
```

---

## Modèles IA

Les modèles ne sont pas inclus. Voir [models/README.md](models/README.md) et [docs/AI_PIPELINE.md](docs/AI_PIPELINE.md) pour les entraîner/exporter. Si un `.onnx` est présent dans `models/` au démarrage, `Application::initializeAI()` l'initialise automatiquement en arrière-plan (flag `ai.enabled`).

| Fichier | Usage |
|---|---|
| `component_detector.onnx` | Détection de composants (YOLOv8 / RT-DETR) — **câblé** |
| `solder_inspector.onnx` | Qualité des soudures — non câblé |
| `ocr_model.onnx` | Lecture des marquages composants — non câblé |

---

## Tests

```bash
cd build && ctest --output-on-failure
```

---

## Roadmap

- [x] Capture caméra USB 1920×1080@30fps + RealSense D405 (couleur + profondeur)
- [x] Import iBOM + overlay pads / silkscreen / labels
- [x] Calibration checkerboard + PDF patron + moniteur de qualité
- [x] Alignement 4 points / 2 composants / ancrage 1-point / **Auto-Align**
- [x] Live tracking ORB avec masquage zone carte
- [x] Scale dynamique px/mm + support bagues optiques
- [x] Settings complets + thème dark/light + nuage de points 3D
- [x] Dataset Creator (capture + auto-annotation YOLO)
- [x] Migration Docker / Jetson AGX Orin (en cours de stabilisation)
- [ ] Intégration complète modèle IA (détection composants)
- [ ] Inspection soudure temps réel
- [ ] Export rapport PDF
- [ ] Release v1.0

---

## Licence

MIT — voir [LICENSE](LICENSE).
