# MicroscopeIBOM

**Microscope USB + IA + InteractiveHtmlBom — Inspection PCB en temps réel**

Application C++ qui capture le flux d'un microscope USB, détecte les composants par IA (YOLOv8/ONNX Runtime) et superpose les informations d'un fichier [InteractiveHtmlBom](https://github.com/openscopeproject/InteractiveHtmlBom) directement sur l'image live.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus)
![Qt6](https://img.shields.io/badge/Qt-6.6+-green?logo=qt)
![OpenCV](https://img.shields.io/badge/OpenCV-4.9+-orange?logo=opencv)
![ONNX Runtime](https://img.shields.io/badge/ONNX_Runtime-1.17+-purple)
![TensorRT](https://img.shields.io/badge/TensorRT-10.x-76B900?logo=nvidia)
![License](https://img.shields.io/badge/License-MIT-yellow)
![Platform](https://img.shields.io/badge/Platform-Windows%20|%20Linux-lightgrey)

---

## Fonctionnalités

| Module | Description |
|---|---|
| **Camera** | Capture USB temps réel (DirectShow/V4L2), calibration checkerboard, triple-buffer |
| **iBOM Parser** | Import direct de fichiers `.html` InteractiveHtmlBom, index spatial O(1) |
| **IA Detection** | Détection composants (YOLOv8), inspection soudure (6 classes), OCR marquages |
| **Overlay** | Superposition PCB↔caméra par homographie RANSAC, couleurs par état |
| **Pick & Place** | Workflow guidé de placement composant par composant |
| **Mesure** | Distance, angle, aire calibrés en mm sur l'image live |
| **Heatmap** | Carte thermique des défauts détectés |
| **Barcode/QR** | Scan de codes-barres/QR pour identifier les composants |
| **Voice Control** | Commandes vocales mains-libres |
| **Remote View** | Streaming WebSocket pour visualisation à distance |
| **Export** | Rapports PDF/HTML, export CSV/JSON/BOM |
| **Snapshots** | Historique de captures avec annotations par composant |

---

## Capture d'écran

> *À venir — l'application est en phase de scaffold initial.*

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
| Langage | C++20 | GCC 13+ / MSVC 19.x |
| GUI | Qt6 (Widgets, OpenGL, Multimedia, WebSockets) | 6.6+ |
| Vision | OpenCV | 4.9+ |
| Inférence | ONNX Runtime + TensorRT EP + CUDA EP | 1.17+ |
| GPU | NVIDIA CUDA + TensorRT | 12.x / 10.x |
| Build | CMake + Ninja | 3.28+ |
| Packages | vcpkg | latest |
| Logging | spdlog | 1.x |
| JSON | nlohmann/json | 3.x |
| Barcode | ZXing-cpp | 2.x |
| PDF | libharu (optionnel) | 2.4+ |
| Tests | Catch2 | 3.5+ |

---

## Prérequis

- **Windows 10/11** (ou Linux Ubuntu 22.04+)
- **GPU NVIDIA** avec drivers récents (optimisé pour RTX 5070)
- **Visual Studio 2022** Build Tools ou Community
- **Qt6 6.6+** ([télécharger](https://www.qt.io/download))
- **CUDA Toolkit 12.x** ([télécharger](https://developer.nvidia.com/cuda-downloads))
- **cuDNN 9.x** ([télécharger](https://developer.nvidia.com/cudnn))
- **TensorRT 10.x** ([télécharger](https://developer.nvidia.com/tensorrt)) *(optionnel)*

---

## Installation rapide (Windows)

### Tout-en-un

```bat
:: Lancer en administrateur pour installer les prérequis + compiler
build_windows.bat
```

### Étape par étape

```bat
:: 1. Installer les prérequis (admin requis)
scripts\install_prerequisites.bat

:: 2. Installer Qt6 manuellement (Qt Online Installer)
::    Définir la variable d'environnement :
set Qt6_DIR=C:\Qt\6.8.0\msvc2022_64\lib\cmake\Qt6

:: 3. Compiler
build_windows.bat --skip-install
```

### Options du build script

```
build_windows.bat --help
  --skip-install    Sauter l'installation des prérequis
  --skip-vcpkg      Sauter les dépendances vcpkg
  --release         Build Release (défaut: RelWithDebInfo)
  --debug           Build Debug
  --clean           Supprimer build/ avant compilation
  --no-tensorrt     Désactiver TensorRT
  --tests           Compiler et exécuter les tests
```

---

## Installation (Linux)

```bash
# 1. Prérequis
chmod +x scripts/install_prerequisites.sh
sudo scripts/install_prerequisites.sh

# 2. Compiler
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel $(nproc)
```

---

## Utilisation

```bash
# Lancer l'application
./build/bin/MicroscopeIBOM

# Avec un fichier iBOM
./build/bin/MicroscopeIBOM --ibom "path/to/board.html"

# Avec une caméra spécifique + mode sombre
./build/bin/MicroscopeIBOM --camera 1 --dark

# Drag & drop : glissez un fichier .html directement dans la fenêtre
```

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
| `Ctrl+C` | Connecter/déconnecter caméra |
| `Space` | Capturer snapshot |
| `F5` | Lancer inspection automatique |
| `Tab` | Composant suivant (mode P&P) |
| `Shift+Tab` | Composant précédent |
| `Ctrl++` / `Ctrl+-` | Zoom in / out |
| `Ctrl+D` | Basculer thème dark/light |
| `Ctrl+F` | Rechercher composant |
| `Ctrl+E` | Exporter rapport |
| `Escape` | Annuler mode en cours |

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
- [ ] Première compilation réussie
- [ ] Capture caméra fonctionnelle
- [ ] Import iBOM + overlay basique
- [ ] Intégration modèle IA PCB
- [ ] Inspection soudure temps réel
- [ ] Mode pick & place guidé
- [ ] Export rapport PDF
- [ ] Release v1.0

---

## Licence

MIT — voir [LICENSE](LICENSE) pour les détails.

---

*Développé pour l'inspection PCB avec microscope USB et intelligence artificielle.*
