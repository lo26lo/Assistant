# MicroscopeIBOM

**Microscope USB + IA + InteractiveHtmlBom — Inspection PCB en temps réel**

Application C++ qui capture le flux d'un microscope USB, détecte les composants par IA (YOLOv8/ONNX Runtime) et superpose les informations d'un fichier [InteractiveHtmlBom](https://github.com/openscopeproject/InteractiveHtmlBom) directement sur l'image live.

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

### Opérationnelles ✅

| Module | Description |
|---|---|
| **Caméra USB** | Capture temps réel 1920×1080@30fps (MSMF backend), LED auto |
| **Calibration** | Checkerboard configurable (taille carte/carrés dans Settings), undistortion temps réel |
| **Scale dynamique** | px/mm mis à jour auto quand le zoom du microscope change (2 méthodes : homographie ou pads iBOM, choix dans Settings) |
| **iBOM Parser** | Import fichiers `.html` InteractiveHtmlBom, supporte JSON direct et LZ-String compressé |
| **Overlay PCB** | Superposition pads + silkscreen + labels ref sur flux caméra, via homographie RANSAC |
| **Alignement manuel** | 4 clics sur les coins du PCB → homographie calculée |
| **Live Tracking** | Suivi ORB feature matching + homographie dynamique, s'adapte au déplacement et zoom |
| **BOM Panel** | Tableau composants avec sélection → highlight overlay |
| **Heatmap** | Toggle carte thermique des défauts |
| **Settings** | Dialog 4 onglets (Camera/Overlay/Tracking/AI), persistance JSON |
| **Thème Dark/Light** | Stylesheets complètes, couleurs centralisées (Theme.h) |
| **Camera Fullscreen** | Double-clic → plein écran caméra seule, Escape pour revenir |
| **Screenshot** | Menu File → Save capture |
| **FPS & Stats** | Timer 1s → StatsPanel + statusBar, scale px/mm en temps réel |
| **Inspection Wizard** | Workflow guidé 4 étapes |

### À connecter ❌

| Module | Description |
|---|---|
| **IA Détection** | YOLOv8/ONNX Runtime + TensorRT — compilé mais pas de modèle .onnx |
| **OCR** | Lecture marquages composants — code prêt, pas de modèle |
| **Solder Inspector** | Inspection qualité soudure — code prêt, pas de modèle |
| **Pick & Place** | Workflow guidé de placement — code prêt, non instancié |
| **Mesure** | Distance/angle/aire calibrés en mm — code prêt, non instancié |
| **Barcode/QR** | Scan codes-barres/QR — code prêt, non instancié |
| **Voice Control** | Commandes vocales mains-libres — code prêt, non instancié |
| **Remote View** | Streaming WebSocket — code prêt, non instancié |
| **Export** | Rapports PDF/HTML, CSV/JSON — code prêt, non instancié |
| **Snapshots** | Historique captures avec annotations — code prêt, non instancié |

---

## Capture d'écran

> *À venir — l'application est fonctionnelle, captures à documenter.*

---

## Architecture

```
┌──────────────────────────────────────────────────┐
│                   Qt6 GUI                        │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────┐ │
│  │ BomPanel │ │  Camera  │ │  ControlPanel    │ │
│  │          │ │   View   │ │  StatsPanel      │ │
│  └──────────┘ └────┬─────┘ └──────────────────┘ │
├────────────────────┼─────────────────────────────┤
│                    │                             │
│  ┌─────────┐  ┌────▼─────┐  ┌────────────────┐  │
│  │  iBOM   │  │ Overlay  │  │  AI Engine     │  │
│  │ Parser  │◄─┤ Renderer ├──┤ ONNX Runtime   │  │
│  │ (JSON)  │  │(QPainter)│  │ + TensorRT     │  │
│  └─────────┘  └──────────┘  └────────────────┘  │
│                                                  │
│  ┌─────────┐  ┌──────────┐  ┌────────────────┐  │
│  │ Camera  │  │ Features │  │    Export       │  │
│  │ Capture │  │ P&P/Voice│  │  PDF/CSV/JSON  │  │
│  │ OpenCV  │  │ Barcode  │  │                │  │
│  └─────────┘  └──────────┘  └────────────────┘  │
└──────────────────────────────────────────────────┘
```

---

## Stack technique

| Composant | Technologie | Version |
|---|---|---|
| Langage | C++20 | MSVC 14.44 (VS Build Tools 2022 v17.14) |
| GUI | Qt6 (Widgets) | 6.8.2 |
| Vision | OpenCV | 4.12 (vcpkg, MSMF backend) |
| Inférence | ONNX Runtime + TensorRT EP + CUDA EP | 1.23.2 |
| GPU | NVIDIA CUDA + cuDNN + TensorRT | 13.2 / 9.20 / 10.15.1 |
| Build | CMake + NMake Makefiles | CMake 4.2.3 |
| Packages | vcpkg | latest |
| Logging | spdlog | 1.x |
| JSON | nlohmann/json | 3.x |
| Barcode | ZXing-cpp | 2.x |
| PDF | libharu (optionnel) | 2.4+ |
| Tests | Catch2 | 3.5+ |

---

## Prérequis

- **Windows 10/11**
- **GPU NVIDIA** avec drivers récents (optimisé pour RTX 5070 Laptop)
- **Visual Studio 2022 Build Tools** (pas Community)
- **Qt6 6.8+** ([télécharger](https://www.qt.io/download))
- **CUDA Toolkit 13.x** ([télécharger](https://developer.nvidia.com/cuda-downloads))
- **cuDNN 9.x** ([télécharger](https://developer.nvidia.com/cudnn))
- **TensorRT 10.x** ([télécharger](https://developer.nvidia.com/tensorrt)) *(optionnel)*

---

## Installation rapide (Windows)

### Tout-en-un

```bat
:: Lancer en administrateur pour installer les prérequis + compiler
build_windows.bat
```

### Build manuel

```bat
:: 1. Ouvrir un terminal et configurer MSVC
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set TENSORRT_HOME=C:\TensorRT\TensorRT-10.15.1.29
set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2

:: 2. Configurer CMake (une seule fois)
cmake -B build -S . -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE=C:/Tools/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DIBOM_ENABLE_TENSORRT=ON ^
  -DQt6_DIR=C:/Qt/6.8.2/msvc2022_64/lib/cmake/Qt6

:: 3. Compiler
cd build
nmake
```

> ⚠️ **NE PAS utiliser Ninja** comme générateur — bug connu avec CMake 4.2.3 (`rules.ninja`).
> Utiliser `NMake Makefiles` uniquement.

> ⚠️ **NE PAS supprimer** `build\vcpkg_installed\` — onnxruntime CUDA+TRT prend ~2h à compiler.

---

## Utilisation

```bash
# Lancer l'application
cd build\bin
.\MicroscopeIBOM.exe
```

### Workflow typique

1. Lancer l'app, cliquer **Start Camera**
2. **File → Open iBOM** → charger un fichier `.html` InteractiveHtmlBom
3. Cliquer **Set Alignment** → cliquer les 4 coins du PCB (TL, TR, BR, BL)
4. L'overlay des composants apparaît sur l'image caméra
5. Activer **Live Tracking** pour que l'overlay suive les mouvements
6. Monter/descendre le microscope → le scale (px/mm) se met à jour auto
7. Configurer via **Ctrl+,** (Settings) : calibration, tracking, overlay, AI

---

## Structure du projet

```
Assistant/
├── CMakeLists.txt              # Build principal
├── vcpkg.json                  # Dépendances C++
├── build_windows.bat           # Script tout-en-un Windows
├── cmake/
│   ├── CompilerFlags.cmake     # Flags MSVC/GCC/Clang
│   └── FindTensorRT.cmake      # Détection TensorRT
├── src/
│   ├── main.cpp                # Point d'entrée
│   ├── app/                    # Application, Config
│   ├── camera/                 # Capture, Calibration, FrameBuffer
│   ├── ibom/                   # Parser HTML, Données, Index spatial
│   ├── ai/                     # ONNX Runtime, Détection, Soudure, OCR
│   ├── overlay/                # Homographie, Rendu overlay, Heatmap
│   ├── gui/                    # MainWindow, CameraView, Panels
│   ├── features/               # P&P, Voice, Barcode, Mesure, Remote
│   ├── export/                 # PDF/HTML/CSV exporters
│   └── utils/                  # Logger, GPU, Image utils
├── tests/                      # Tests unitaires Catch2
├── models/                     # Modèles IA (.onnx)
├── resources/                  # Icônes, QRC
├── scripts/                    # Installeurs Windows/Linux
└── docs/                       # Documentation projet
```

**88 fichiers** — 71 sources C++, 5 tests, 4 build, 8 support — **11 400+ lignes de code**

---

## Modèles IA

L'application nécessite des modèles ONNX entraînés. Voir [models/README.md](models/README.md) pour :

- Entraîner un YOLOv8 sur un dataset PCB
- Exporter en ONNX avec optimisations TensorRT
- Format des fichiers de classes

Modèles attendus dans `models/` :
| Fichier | Usage |
|---|---|
| `component_detector.onnx` | Détection de composants |
| `solder_inspector.onnx` | Inspection qualité soudure |
| `ocr_model.onnx` | Lecture marquages composants |

---

## Raccourcis clavier

| Raccourci | Action |
|---|---|
| `Ctrl+O` | Ouvrir fichier iBOM |
| `Ctrl+,` | Ouvrir Settings |
| `Ctrl+S` | Sauvegarder capture d'écran |
| `Double-clic` | Plein écran caméra (Escape pour revenir) |
| `Escape` | Annuler mode en cours / quitter fullscreen |

---

## Contribuer

1. Fork le repo
2. Créer une branche (`git checkout -b feature/ma-feature`)
3. Commit (`git commit -m "feat: description"`)
4. Push (`git push origin feature/ma-feature`)
5. Ouvrir une Pull Request

---

## Roadmap

- [x] Scaffold complet (88 fichiers, 9 modules)
- [x] Compilation réussie (NMake + MSVC + vcpkg)
- [x] Capture caméra fonctionnelle (1920×1080@30fps)
- [x] Import iBOM + overlay pads/silkscreen/labels
- [x] Calibration caméra checkerboard (configurable)
- [x] Alignement manuel (4 points) + live tracking ORB
- [x] Scale dynamique (auto-zoom px/mm)
- [x] Settings dialog (Camera/Overlay/Tracking/AI)
- [x] Thème dark/light harmonisé
- [ ] Intégration modèle IA (détection composants)
- [ ] Inspection soudure temps réel
- [ ] Mode pick & place guidé
- [ ] Export rapport PDF
- [ ] Release v1.0

---

## Licence

MIT — voir [LICENSE](LICENSE) pour les détails.

---

*Développé pour l'inspection PCB avec microscope USB et intelligence artificielle.*
