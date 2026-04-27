# MicroscopeIBOM

Outil d'inspection PCB : microscope USB + overlay [InteractiveHtmlBom](https://github.com/openscopeproject/InteractiveHtmlBom) + inférence IA (ONNX Runtime / TensorRT).

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus)
![Qt6](https://img.shields.io/badge/Qt-6.8.2-green?logo=qt)
![OpenCV](https://img.shields.io/badge/OpenCV-4.12-orange?logo=opencv)
![ONNX Runtime](https://img.shields.io/badge/ONNX_Runtime-1.23.2-purple)
![TensorRT](https://img.shields.io/badge/TensorRT-10.15-76B900?logo=nvidia)
![CUDA](https://img.shields.io/badge/CUDA-13.2-76B900?logo=nvidia)
![License](https://img.shields.io/badge/License-MIT-yellow)
![Platform](https://img.shields.io/badge/Platform-Windows-lightgrey)

---

## Fonctionnalités

### Opérationnelles

| Module | Description |
|---|---|
| **Caméra USB** | Capture temps réel 1920×1080@30fps (MSMF), sélecteur de périphérique |
| **Calibration** | Checkerboard configurable, undistortion temps réel, PDF patron intégré |
| **iBOM Parser** | Import `.html` InteractiveHtmlBom — JSON direct et LZ-String compressé |
| **Overlay PCB** | Pads, silkscreen, labels ref superposés sur le flux caméra via homographie |
| **Alignement** | 4 clics (coins du PCB) ou 2 composants de référence (transformation similarité) |
| **Live Tracking** | ORB feature matching + RANSAC dans un thread dédié, downscale configurable |
| **Scale dynamique** | px/mm mis à jour automatiquement (méthode : homographie ou pads iBOM) |
| **Bagues optiques** | Multiplicateur 0.5×–2× appliqué au scale px/mm |
| **BOM Panel** | Tableau composants avec sélection → highlight sur l'overlay |
| **Heatmap** | Carte thermique des défauts (toggle) |
| **Settings** | Dialog 4 onglets (Camera / Overlay / Tracking / AI), persistance JSON |
| **Thème** | Dark / Light (Catppuccin Mocha / Latte), couleurs centralisées dans `Theme.h` |
| **Fullscreen** | Double-clic → plein écran caméra, Escape pour revenir |
| **Inspection Wizard** | Workflow guidé 4 étapes |
| **Help** | 8 sections : Démarrage, Calibration, Alignement, Lentilles, Inspection, Overlay, Export, Dépannage |

### En attente de modèles / non instancié

| Module | État |
|---|---|
| **IA Détection** | Compilé — YOLOv8/RT-DETR via ONNX Runtime + TensorRT, sans modèle `.onnx` |
| **OCR** | Code prêt, pas de modèle |
| **Solder Inspector** | Code prêt, pas de modèle |
| **Pick & Place** | Code prêt, non instancié |
| **Mesure** | Distance / angle / aire en mm — code prêt, non instancié |
| **Barcode / QR** | Code prêt, non instancié |
| **Voice Control** | Commandes vocales mains-libres — code prêt, non instancié |
| **Remote View** | Streaming WebSocket — code prêt, non instancié |
| **Export** | PDF / HTML / CSV / JSON — code prêt, non instancié |
| **Snapshots** | Historique annoté — code prêt, non instancié |

---

## Architecture

```
main.cpp
  └─ QApplication
  └─ Application (QObject)
       ├─ Config              JSON, %APPDATA%\MicroscopeIBOM\
       ├─ CameraCapture       thread → frameReady(FrameRef) → CameraView
       ├─ CameraCalibration   YAML, undistort
       ├─ IBomParser          HTML → JSON → IBomProject
       ├─ OverlayRenderer     pads + silkscreen + labels (QPainter)
       ├─ Homography          pcbToImage, transformRect
       ├─ HeatmapRenderer
       ├─ TrackingWorker      thread dédié — ORB + RANSAC
       └─ MainWindow
            ├─ CameraView     paintEvent, zoom/pan, fullscreen
            ├─ ControlPanel   toggles, sliders
            ├─ BomPanel       QTableWidget, filtres, checkboxes
            ├─ StatsPanel     FPS, GPU, scale px/mm
            ├─ SettingsDialog 4 onglets
            ├─ HelpDialog     8 onglets
            └─ InspectionWizard
```

### Modèle de threading

| Thread | Rôle |
|---|---|
| Main / GUI | Qt event loop, paintEvent, signals/slots UI |
| CameraCapture | `captureLoop()` — lit `cv::VideoCapture`, émet `frameReady(FrameRef)` |
| TrackingWorker | ORB + RANSAC — reçoit `processFrame(FrameRef)`, émet `homographyUpdated(cv::Mat)` |

`FrameRef = std::shared_ptr<const cv::Mat>` — zero-copy entre les threads.

---

## Stack technique

| Composant | Technologie | Version |
|---|---|---|
| Langage | C++20 | MSVC 14.44 (VS Build Tools 2022) |
| GUI | Qt6 Widgets | 6.8.2 |
| Vision | OpenCV | 4.12 (vcpkg) |
| Inférence | ONNX Runtime (CUDA EP + TensorRT EP) | 1.23.2 |
| GPU | NVIDIA CUDA / cuDNN / TensorRT | 13.2 / 9.20 / 10.15.1 |
| Build | CMake + NMake Makefiles | CMake 4.2.3 |
| Packages | vcpkg | — |
| Logging | spdlog | 1.x |
| JSON | nlohmann/json | 3.x |
| Tests | Catch2 | 3.5+ |

---

## Prérequis

- Windows 10 / 11
- GPU NVIDIA avec drivers récents
- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022) (onglet *Build Tools*, pas Community)
- [Qt 6.8+](https://www.qt.io/download) — variante `msvc2022_64`
- [CUDA Toolkit 13.x](https://developer.nvidia.com/cuda-downloads)
- [cuDNN 9.x](https://developer.nvidia.com/cudnn)
- [TensorRT 10.x](https://developer.nvidia.com/tensorrt) *(optionnel)*
- [vcpkg](https://github.com/microsoft/vcpkg) installé dans `C:\Tools\vcpkg`

---

## Build

### 1. Environnement

```bat
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set TENSORRT_HOME=C:\TensorRT\TensorRT-10.15.1.29
set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
```

### 2. Configuration CMake (une seule fois)

```bat
cmake -B build -S . -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE=C:/Tools/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DIBOM_ENABLE_TENSORRT=ON ^
  -DQt6_DIR=C:/Qt/6.8.2/msvc2022_64/lib/cmake/Qt6
```

### 3. Compilation

```bat
cd build
nmake
```

Ou en parallèle :

```bat
cmake --build build -j 8
```

> **NE PAS utiliser Ninja** — bug `rules.ninja` avec CMake 4.2.3. Utiliser `NMake Makefiles` uniquement.

> **NE PAS supprimer** `build\vcpkg_installed\` — onnxruntime CUDA+TRT prend environ 2h à recompiler.

---

## Lancement

```bat
cd build\bin
.\MicroscopeIBOM.exe
```

### Workflow typique

1. **Start Camera** — sélectionner le périphérique dans le panneau latéral
2. Si bague optique : **Settings → Camera → Lens Adapter** (0.5×–2×)
3. **Camera → Open Calibration Patterns PDF** → imprimer et calibrer (5 captures minimum)
4. **File → Open iBOM** → charger un fichier `.html` InteractiveHtmlBom
5. **Set Alignment** → cliquer les 4 coins du PCB (ou 2 composants en mode rapide)
6. Activer **Live Tracking** pour que l'overlay suive les déplacements
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
├── build_windows.bat
├── cmake/
│   ├── CompilerFlags.cmake
│   └── FindTensorRT.cmake
├── src/
│   ├── main.cpp
│   ├── app/          Application, Config
│   ├── camera/       CameraCapture, CameraCalibration, FrameBuffer
│   ├── ibom/         IBomParser, IBomData, ComponentMap
│   ├── overlay/      Homography, OverlayRenderer, HeatmapRenderer, TrackingWorker
│   ├── gui/          MainWindow, CameraView, BomPanel, ControlPanel, StatsPanel, Dialogs
│   ├── ai/           InferenceEngine, ComponentDetector, OCREngine, SolderInspector
│   ├── features/     PickAndPlace, Measurement, VoiceControl, BarcodeScanner, RemoteView
│   ├── export/       ReportGenerator, DataExporter
│   └── utils/        Logger, GpuUtils, ImageUtils
├── tests/            Catch2 — ibom_parser, homography, inference, component_matching
├── models/           Modèles ONNX (voir models/README.md)
├── resources/
└── docs/
```

---

## Modèles IA

Les modèles ne sont pas inclus. Voir [models/README.md](models/README.md) pour les entraîner et les exporter.

| Fichier | Usage |
|---|---|
| `component_detector.onnx` | Détection de composants (YOLOv8 / RT-DETR) |
| `solder_inspector.onnx` | Qualité des soudures |
| `ocr_model.onnx` | Lecture des marquages composants |

---

## Tests

```bat
cd build
ctest --output-on-failure
```

---

## Roadmap

- [x] Capture caméra 1920×1080@30fps
- [x] Import iBOM + overlay pads / silkscreen / labels
- [x] Calibration checkerboard configurable + PDF patron
- [x] Alignement 4 points + mode 2 composants + live tracking ORB
- [x] Scale dynamique px/mm + support bagues optiques
- [x] Settings complets + thème dark/light
- [ ] Intégration modèle IA (détection composants)
- [ ] Inspection soudure temps réel
- [ ] Mode pick & place guidé
- [ ] Export rapport PDF
- [ ] Release v1.0

---

## Licence

MIT — voir [LICENSE](LICENSE).
