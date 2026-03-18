# MicroScope iBOM AI Overlay — Documentation Projet

> **Date de création :** 2026-03-18  
> **Architecture choisie :** Proposition B — C++ / Qt6 + ONNX Runtime (TensorRT)  
> **Raison du choix :** Latence minimale requise pour l'overlay temps réel  

---

## 1. Contexte & Objectif

Application PC (portable avec RTX 5070) qui :
- Capture le flux vidéo USB d'un microscope
- Affiche le flux en temps réel avec overlay
- Parse les fichiers InteractiveHtmlBom (iBOM) pour extraire composants, coordonnées, BOM
- Utilise l'IA (inférence GPU) pour détecter et identifier les composants sur le PCB
- Superpose les informations iBOM sur la vue caméra en temps réel

---

## 2. Propositions évaluées

### Proposition A — Python + OpenCV + Qt (Non retenue)

| Couche | Techno |
|---|---|
| Capture caméra | OpenCV `VideoCapture` (USB) |
| GUI | PySide6 / PyQt6 |
| IA détection | YOLOv8 ou RT-DETR (Ultralytics) via CUDA (RTX 5070) |
| Alignement PCB | Homographie OpenCV (détection de coins/fiducials) |
| Overlay | QPainter ou OpenCV `addWeighted` |
| Parsing iBOM | BeautifulSoup + JSON extraction |

**Flux :** Caméra USB → OpenCV frame → modèle IA (GPU) → overlay composants iBOM → affichage Qt

**Avantages :** Écosystème Python riche, prototypage rapide, excellente intégration CUDA via PyTorch/ONNX Runtime GPU.

**Inconvénients :** Latence plus élevée due à Python GIL, overhead d'interprétation.

### Proposition B — C++ / Qt6 + ONNX Runtime (✅ RETENUE)

| Couche | Techno |
|---|---|
| Capture | Qt6 Multimedia / DirectShow / V4L2 (Linux) |
| GUI | Qt6 Widgets + QGraphicsScene / QML |
| IA | ONNX Runtime C++ avec TensorRT EP (RTX 5070) |
| Vision | OpenCV C++ (homographie, calibration) |
| Overlay | QGraphicsScene + OpenGL / QPainter |
| Parsing iBOM | nlohmann/json + regex (extraction JSON du HTML) |
| Build | CMake + vcpkg (cross-platform) |

**Flux :** Caméra USB → capture native → OpenCV Mat → ONNX Runtime TensorRT (GPU) → overlay Qt → affichage

**Avantages :** Performance native maximale, latence minimale (<5ms overlay), contrôle total du pipeline GPU.

**Inconvénients :** Temps de développement plus long, complexité du build chain.

### Proposition C — Electron + Python backend (Non retenue)

| Couche | Techno |
|---|---|
| Frontend | Electron (réutilise le rendu iBOM HTML/JS existant) |
| Backend | Python FastAPI (capture caméra + IA) |
| Communication | WebSocket (stream MJPEG ou frames base64) |

**Avantage clé :** Réutilisation directe du viewer iBOM.

**Inconvénients :** Latence réseau local, consommation mémoire Electron, pas adapté au temps réel strict.

---

## 3. Stack technique finale (Proposition B)

```
┌─────────────────────────────────────────────────────┐
│                    Application Qt6                    │
│  ┌──────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │ Camera    │  │  AI Engine   │  │  iBOM Parser  │  │
│  │ Capture   │→ │  ONNX+TRT   │→ │  JSON Data    │  │
│  │ Module    │  │  (GPU)       │  │               │  │
│  └──────────┘  └──────────────┘  └───────────────┘  │
│       ↓              ↓                   ↓           │
│  ┌───────────────────────────────────────────────┐   │
│  │           Overlay Renderer                     │   │
│  │     QGraphicsScene + OpenGL Compositing        │   │
│  └───────────────────────────────────────────────┘   │
│       ↓                                              │
│  ┌───────────────────────────────────────────────┐   │
│  │              Main Window                       │   │
│  │  Camera View | BOM Panel | Controls | Stats    │   │
│  └───────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

### Dépendances principales

| Dépendance | Version min | Usage |
|---|---|---|
| Qt6 | 6.6+ | GUI, Multimedia, OpenGL |
| OpenCV | 4.9+ | Vision, calibration, homographie |
| ONNX Runtime | 1.17+ | Inférence IA |
| TensorRT | 10.x | Accélération GPU NVIDIA |
| CUDA Toolkit | 12.x | Support GPU |
| cuDNN | 9.x | Opérations deep learning |
| nlohmann/json | 3.11+ | Parsing JSON |
| CMake | 3.28+ | Build system |
| vcpkg | latest | Package manager C++ |
| spdlog | 1.13+ | Logging |
| Catch2 | 3.5+ | Tests unitaires |

---

## 4. Plan de développement

### Phase 1 — Fondations (Semaine 1-2)

| # | Tâche | Description | Priorité |
|---|---|---|---|
| 1.1 | Setup build system | CMake + vcpkg, structure projet, CI basique | CRITIQUE |
| 1.2 | Parsing iBOM | Extraire JSON du HTML (composants, coords, BOM, layers) | CRITIQUE |
| 1.3 | Capture caméra | Afficher flux USB dans fenêtre Qt via Qt Multimedia | CRITIQUE |
| 1.4 | GUI skeleton | Fenêtre principale split : vue caméra + panneau BOM | HAUTE |

### Phase 2 — Calibration & Alignement (Semaine 3-4)

| # | Tâche | Description | Priorité |
|---|---|---|---|
| 2.1 | Détection fiducials | Via OpenCV (contours, template matching, cercles) | CRITIQUE |
| 2.2 | Calibration caméra | Correction distorsion optique du microscope | HAUTE |
| 2.3 | Homographie | Mapper coordonnées iBOM → pixels caméra | CRITIQUE |
| 2.4 | Overlay statique | Dessiner contours composants iBOM sur vue caméra | HAUTE |

### Phase 3 — IA & Détection (Semaine 5-7)

| # | Tâche | Description | Priorité |
|---|---|---|---|
| 3.1 | Préparation dataset | Collecter/annoter images de composants CMS | CRITIQUE |
| 3.2 | Entraînement modèle | YOLOv8-nano ou RT-DETR, export ONNX | CRITIQUE |
| 3.3 | Pipeline inférence | ONNX Runtime C++ + TensorRT EP, >60 FPS | CRITIQUE |
| 3.4 | Matching IA ↔ iBOM | Associer détections visuelles aux composants BOM | HAUTE |

### Phase 4 — Overlay intelligent (Semaine 8-9)

| # | Tâche | Description | Priorité |
|---|---|---|---|
| 4.1 | Overlay contextuel | Afficher Value, Reference, Footprint sur détection | HAUTE |
| 4.2 | Code couleur état | Vert=placé, Rouge=manquant, Orange=mauvaise orientation | HAUTE |
| 4.3 | Sync BOM ↔ Vue | Click dans BOM → highlight caméra et inversement | MOYENNE |
| 4.4 | Détection d'orientation | Vérifier sens du composant (pin 1, polarité) | HAUTE |

### Phase 5 — Features avancées (Semaine 10-14)

| # | Tâche | Description | Priorité |
|---|---|---|---|
| 5.1 | Détection soudure | Analyse joints (pont, soudure froide, insuffisante) | HAUTE |
| 5.2 | Pick & place assisté | Guide pas à pas, ordre optimal de placement | HAUTE |
| 5.3 | OCR composants | Lire marquages IC, vérifier vs BOM | MOYENNE |
| 5.4 | Mesure sur image | Calibrer échelle, mesurer distances | MOYENNE |
| 5.5 | Composants manquants | Comparer emplacements iBOM vs détections IA | HAUTE |
| 5.6 | Multi-layer toggle | Basculer front/back avec overlay adapté | MOYENNE |

### Phase 6 — Polish & Extras (Semaine 15-18)

| # | Tâche | Description | Priorité |
|---|---|---|---|
| 6.1 | Historique & diff | Snapshots avant/après, comparaison pour rework | MOYENNE |
| 6.2 | Heatmap défauts | Agrégation défauts sur plusieurs PCB | BASSE |
| 6.3 | Commandes vocales | "next component", "mark as placed" — mains libres | MOYENNE |
| 6.4 | Barcode/QR scan | Scanner code PCB → charger bon fichier iBOM | BASSE |
| 6.5 | Export rapport | PDF avec captures annotées, défauts, conformité | HAUTE |
| 6.6 | Mode training IA | Capturer/annoter depuis l'app pour améliorer modèle | MOYENNE |
| 6.7 | Remote view | Stream vers navigateur web (review/formation) | BASSE |
| 6.8 | Stencil alignment | Aider alignement stencil pâte à braser via overlay | BASSE |
| 6.9 | Zoom digital sync | Synchroniser zoom microscope ↔ zoom overlay | MOYENNE |
| 6.10 | Mode inspection | Parcours automatique de la BOM composant par composant | MOYENNE |

---

## 5. Features complètes

### 5.1 Core Features

| Feature | Description |
|---|---|
| **Capture caméra USB** | Flux vidéo temps réel depuis microscope USB |
| **Parsing iBOM** | Extraction automatique de toutes les données du HTML iBOM |
| **Overlay composants** | Superposition des contours/infos iBOM sur la vue caméra |
| **Détection IA composants** | Identification automatique des composants CMS par IA |
| **BOM interactive** | Panneau BOM synchronisé avec la vue caméra |
| **Calibration/Homographie** | Alignement précis entre coordonnées iBOM et vue caméra |

### 5.2 Features IA avancées

| Feature | Description |
|---|---|
| **Détection d'orientation** | Vérifie que le composant est dans le bon sens (pin 1, polarité) — overlay rouge si inversé |
| **Détection de soudure** | Analyse des joints de soudure (pont, soudure froide, insuffisante) via vision IA |
| **OCR sur composants** | Lire les marquages sur les IC/composants et vérifier correspondance BOM |
| **Détection composants manquants** | Comparer emplacements iBOM vs détections IA → liste manquants |
| **Mode training IA** | Capturer et annoter des images directement depuis l'app pour améliorer le modèle |

### 5.3 Features UX

| Feature | Description |
|---|---|
| **Pick & place assisté** | Surligne le prochain composant à placer, guide pas à pas dans l'ordre optimal |
| **Mode inspection** | Parcours automatique de la BOM composant par composant |
| **Mesure sur image** | Calibrer l'échelle et mesurer des distances sur la vue microscope |
| **Zoom digital sync** | Synchronisation zoom microscope ↔ zoom overlay |
| **Multi-layer toggle** | Basculer front/back avec overlay adapté (comme iBOM mais sur vue réelle) |
| **Commandes vocales** | "next component", "mark as placed" — mains libres sous microscope |

### 5.4 Features Data & Export

| Feature | Description |
|---|---|
| **Historique & diff** | Capturer snapshots et comparer avant/après (rework) |
| **Heatmap de défauts** | Agrégation défauts détectés sur plusieurs PCB, zones problématiques |
| **Export rapport d'inspection** | PDF avec captures annotées, liste défauts, taux conformité |
| **Barcode/QR scan** | Scanner code PCB pour charger automatiquement le bon iBOM |
| **Remote view** | Stream la vue vers un navigateur web (review à distance, formation) |
| **Stencil alignment** | Aide à l'alignement stencil pâte à braser via overlay |

---

## 6. Structure du projet

```
MicroscopeIBOM/
├── CMakeLists.txt                  # Build principal
├── vcpkg.json                      # Dépendances vcpkg
├── cmake/                          # Modules CMake custom
│   ├── FindONNXRuntime.cmake
│   ├── FindTensorRT.cmake
│   └── CompilerFlags.cmake
├── src/
│   ├── main.cpp                    # Point d'entrée
│   ├── app/
│   │   ├── Application.h/cpp       # Classe application principale
│   │   └── Config.h/cpp            # Configuration persistante
│   ├── camera/
│   │   ├── CameraCapture.h/cpp     # Capture flux USB
│   │   ├── CameraCalibration.h/cpp # Calibration optique
│   │   └── FrameBuffer.h/cpp       # Buffer circulaire GPU
│   ├── ibom/
│   │   ├── IBomParser.h/cpp        # Extraction JSON du HTML
│   │   ├── IBomData.h/cpp          # Structures de données
│   │   └── ComponentMap.h/cpp      # Mapping composants ↔ coordonnées
│   ├── ai/
│   │   ├── InferenceEngine.h/cpp   # Pipeline ONNX Runtime + TensorRT
│   │   ├── ComponentDetector.h/cpp # Détection composants
│   │   ├── SolderInspector.h/cpp   # Inspection soudure
│   │   ├── OCREngine.h/cpp         # Lecture marquages
│   │   └── ModelManager.h/cpp      # Gestion des modèles IA
│   ├── overlay/
│   │   ├── OverlayRenderer.h/cpp   # Rendu overlay OpenGL
│   │   ├── Homography.h/cpp        # Transformation coordonnées
│   │   ├── ComponentOverlay.h/cpp  # Overlay par composant
│   │   └── HeatmapRenderer.h/cpp   # Heatmap défauts
│   ├── gui/
│   │   ├── MainWindow.h/cpp        # Fenêtre principale
│   │   ├── CameraView.h/cpp        # Widget vue caméra
│   │   ├── BomPanel.h/cpp          # Panneau BOM interactif
│   │   ├── ControlPanel.h/cpp      # Contrôles utilisateur
│   │   ├── InspectionWizard.h/cpp  # Mode inspection guidée
│   │   └── StatsPanel.h/cpp        # Statistiques & métriques
│   ├── features/
│   │   ├── PickAndPlace.h/cpp      # Assistant pick & place
│   │   ├── VoiceControl.h/cpp      # Commandes vocales
│   │   ├── BarcodeScanner.h/cpp    # Scan QR/barcode
│   │   ├── Measurement.h/cpp       # Mesures sur image
│   │   ├── StencilAlign.h/cpp      # Alignement stencil
│   │   ├── SnapshotHistory.h/cpp   # Historique & diff
│   │   └── RemoteView.h/cpp        # Streaming distant
│   ├── export/
│   │   ├── ReportGenerator.h/cpp   # Génération PDF
│   │   └── DataExporter.h/cpp      # Export données
│   └── utils/
│       ├── Logger.h/cpp            # Logging (spdlog)
│       ├── GpuUtils.h/cpp          # Utilitaires CUDA
│       └── ImageUtils.h/cpp        # Utilitaires image
├── models/                         # Modèles IA pré-entraînés
│   ├── component_detector.onnx
│   ├── solder_inspector.onnx
│   └── ocr_model.onnx
├── resources/
│   ├── icons/
│   ├── qml/                        # Fichiers QML (si utilisé)
│   └── styles/
├── tests/
│   ├── test_ibom_parser.cpp
│   ├── test_homography.cpp
│   ├── test_inference.cpp
│   └── test_component_matching.cpp
├── scripts/
│   ├── install_prerequisites.bat   # Installation Windows
│   ├── install_prerequisites.sh    # Installation Linux
│   └── train_model.py              # Script entraînement
├── docs/
│   ├── PROJECT_PLAN.md             # Ce fichier
│   └── PROJECT_STATE.md            # État du projet
└── data/
    ├── ibom_samples/               # Fichiers iBOM de test
    └── training_data/              # Données d'entraînement IA
```

---

## 7. Cross-platform: Windows vs Linux

### Recommandation : Développer pour les DEUX (via CMake + vcpkg)

| Critère | Windows | Linux |
|---|---|---|
| **Support NVIDIA/CUDA** | ✅ Excellent | ✅ Excellent |
| **TensorRT** | ✅ Disponible | ✅ Disponible |
| **Qt6** | ✅ Natif | ✅ Natif |
| **OpenCV** | ✅ vcpkg | ✅ vcpkg / apt |
| **USB Camera** | DirectShow / WMF | V4L2 |
| **Latence GPU** | ✅ Très bonne | ✅ Légèrement meilleure (moins d'overhead OS) |
| **ONNX Runtime** | ✅ Complet | ✅ Complet |
| **Déploiement** | Installeur MSI/EXE | AppImage / Flatpak |

**Conclusion :** Linux a un léger avantage en latence brute (kernel moins d'overhead, meilleur scheduling GPU). Mais Windows est plus pratique pour un portable. **Développer cross-platform dès le départ** avec CMake coûte peu d'effort supplémentaire et donne le choix.

---

## 8. Risques & Mitigations

| Risque | Impact | Mitigation |
|---|---|---|
| Qualité optique microscope variable | Détection IA dégradée | Calibration + augmentation données |
| Composants trop petits (0201) | Résolution insuffisante | Zoom natif microscope, super-résolution IA |
| Alignement PCB imprécis | Overlay décalé | Multi-fiducials, recalibration continue |
| Dataset entraînement insuffisant | Modèle peu fiable | Transfer learning, augmentation, mode training intégré |
| Latence TensorRT build | Premier lancement lent | Cache des engines TensorRT compilés |

---

*Document vivant — Mis à jour au fur et à mesure de l'avancement du projet.*
