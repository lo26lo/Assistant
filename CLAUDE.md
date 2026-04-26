# MicroscopeIBOM — CLAUDE.md

> PCB inspection tool: USB microscope camera + interactive iBOM overlay + AI inference (ONNX/TensorRT).
> Language: C++20 | GUI: Qt6 6.8.2 | Vision: OpenCV 4.12 | AI: ONNX Runtime 1.23.2 + TensorRT 10.15

---

## Pièges critiques

1. **vcvarsall.bat** → `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\...` (PAS `Community`)
2. **NE PAS supprimer** `build\vcpkg_installed\` — onnxruntime CUDA+TRT a pris ~2h à compiler
3. **NE PAS utiliser** `spdlog::flush_every(seconds(0))` → crash ACCESS_VIOLATION (spin-loop interne)
4. **NE PAS appeler** `setupLogging()` dans Application — déjà fait dans `main.cpp`
5. **NE PAS utiliser** Ninja comme générateur CMake → bug `rules.ninja` avec CMake 4.2.3
6. **Atomic** : utiliser `m_frameCount.exchange(0)` PAS `m_frameCount = 0`
7. **IBomData.h** : `BoardInfo` a `boardBBox` (BBox struct minX/minY/maxX/maxY), PAS `width`/`height`
8. **`parseCommandLine()`** : désactivé — `QCommandLineParser::process()` sur app Windows GUI cause QMessageBox+exit
9. **QApplication** : créé dans `main.cpp` AVANT `Application`. `Application` reçoit `QApplication&`
10. **iBOM JSON coords** : TOUJOURS des arrays `[x, y]`, PAS des objets `{x:..., y:...}`. Utiliser `readPoint()` helper.
11. **iBOM value/footprint** : ces infos sont dans `bom.fields`, PAS dans les objets `footprint`. Cross-référence après `parseBomGroups()`.
12. **Build corruption** : Si `nmake` est interrompu pendant le link, supprimer le `.exe` corrompu puis relancer.
13. **Tuer l'app avant rebuild** : Le linker échoue LNK1104 si `MicroscopeIBOM.exe` est en cours. Toujours `Stop-Process` avant `nmake`.
14. **frameReady** : depuis avril 2026, `FrameRef = shared_ptr<const cv::Mat>` zero-copy. `cv::Mat frame;` déclaré **à l'intérieur** de la boucle capture (jamais réutilisé) pour éviter la corruption de buffer partagé.
15. **BFMatcher crossCheck** : pour `knnMatch(k=2)` il faut `crossCheck=false` — sinon l'appel échoue. Avec `crossCheck=true`, n'utiliser que `match()` simple.
16. **`matchDistanceRatio`** : c'est le **Lowe's ratio** (0.5–0.95, typique 0.75). Migration auto dans `Config::load()` si valeur ≥ 1.0 (ancienne sémantique).
17. **FrameRef métatype Qt** : `Q_DECLARE_METATYPE(ibom::camera::FrameRef)` hors du namespace + `qRegisterMetaType<ibom::camera::FrameRef>("ibom::camera::FrameRef")` dans `initialize()`. Le string doit correspondre **exactement** au type qualifié déclaré dans le signal sinon le signal cross-thread est silencieusement ignoré.
18. **TrackingWorker slots** : les méthodes invoquées via `QMetaObject::invokeMethod` doivent être dans `public slots:` avec `Q_OBJECT`. Slots par valeur, pas par référence (Qt copie les arguments dans la queue).
19. **Arrêt QThread** : destructeur `Application::~Application()` doit appeler `m_trackingThread->quit()` + `m_trackingThread->wait()`. Le worker est détruit via `connect(finished, worker, &QObject::deleteLater)`.
20. **Layer enum** : utiliser `Layer::Front`/`Layer::Back` partout, jamais `== 0` ou `== 1` (BomPanel, PickAndPlace, DataExporter).
21. **Protobuf patch** : `build\vcpkg_installed\...\google\protobuf\message_lite.h` — `struct Undefined;` → `struct Undefined {};`. Ce patch est dans `vcpkg_installed`, pas dans buildtrees. À réappliquer si vcpkg réinstalle protobuf.

---

## Build

```cmd
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set TENSORRT_HOME=C:\TensorRT\TensorRT-10.15.1.29
set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
cd /d C:\Users\bambo\Assistant\Assistant\build
nmake
```

Reconfigure (seulement si `build/` supprimé) :
```cmd
"C:\Program Files\CMake\bin\cmake.exe" -B build -S . -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE=C:/Tools/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DIBOM_ENABLE_TENSORRT=ON ^
  -DQt6_DIR=C:/Qt/6.8.2/msvc2022_64/lib/cmake/Qt6
```

Run : `cd C:\Users\bambo\Assistant\Assistant\build\bin && .\MicroscopeIBOM.exe`

Nettoyage partiel (préserve `vcpkg_installed`) :
```powershell
Remove-Item -Recurse -Force build\CMakeFiles
Remove-Item -Force build\CMakeCache.txt, build\Makefile
```

Paralléliser NMake (mono-thread par défaut) :
```cmd
cmake --build build -j 8
```

---

## Chemins clés

| | |
|-|-|
| Sources | `src/` |
| Build | `build/` |
| Binaire | `build/bin/MicroscopeIBOM.exe` |
| Log app | `build/bin/logs/pcb_inspector.log` |
| Config | `%APPDATA%\MicroscopeIBOM\config.json` |
| Calibration | `%APPDATA%\MicroscopeIBOM\calibration.yml` |
| Qt6 | `C:\Qt\6.8.2\msvc2022_64` |
| vcpkg | `C:\Tools\vcpkg` |
| CUDA | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2` |
| TensorRT | `C:\TensorRT\TensorRT-10.15.1.29` |
| Python | `C:\Data\Python312\python.exe` |

---

## Architecture

```
main.cpp
  └─ QApplication
  └─ Application (QObject)
       ├─ Config (JSON, AppData)
       ├─ CameraCapture (thread séparé → frameReady(FrameRef) → CameraView)
       ├─ CameraCalibration (YAML, undistort)
       ├─ IBomParser (HTML → JSON → IBomProject, supporte LZ-String)
       ├─ OverlayRenderer (pads + silkscreen + labels sur QImage)
       ├─ Homography (pcbToImage, transformRect, setMatrix)
       ├─ HeatmapRenderer (heatmap défauts)
       ├─ TrackingWorker (QThread dédié ORB+RANSAC, downscale 0.5×)
       └─ MainWindow
            ├─ CameraView (paintEvent, overlay opacity, double-clic fullscreen, zoom/pan)
            ├─ ControlPanel (toggles, sliders)
            ├─ BomPanel (QTableWidget, filtres, checkboxes)
            ├─ StatsPanel (FPS, GPU, scale px/mm)
            ├─ SettingsDialog (4 onglets: Camera/Overlay/Tracking/AI)
            ├─ HelpDialog (8 onglets)
            └─ InspectionWizard (4 étapes)
```

### Modèle de threading

| Thread | Rôle |
|--------|------|
| Main/GUI | Qt event loop, paintEvent, signals/slots UI |
| CameraCapture | `captureLoop()` — lit `cv::VideoCapture`, émet `frameReady(FrameRef)` en QueuedConnection |
| TrackingWorker | ORB+RANSAC — reçoit `processFrame(FrameRef)` via QueuedConnection, émet `homographyUpdated(cv::Mat)` |

Zero-copy : `FrameRef = std::shared_ptr<const cv::Mat>`. La frame allouée dans le thread capture est partagée sans clone jusqu'à CameraView. Le calibrateur fait un `.clone()` explicite pour stockage long-terme.

---

## Modules source (`src/`)

| Module | Fichiers clés | Connecté |
|--------|--------------|----------|
| `app/` | `Application.h/.cpp`, `Config.h/.cpp` | ✅ |
| `camera/` | `CameraCapture.h/.cpp`, `CameraCalibration.h/.cpp`, `FrameBuffer.h/.cpp` | ✅ |
| `ibom/` | `IBomData.h`, `IBomParser.h/.cpp`, `ComponentMap.h/.cpp` | ✅ |
| `overlay/` | `Homography.h/.cpp`, `OverlayRenderer.h/.cpp`, `ComponentOverlay.h/.cpp`, `HeatmapRenderer.h/.cpp`, `TrackingWorker.h/.cpp` | ✅ |
| `gui/` | `MainWindow`, `CameraView`, `BomPanel`, `ControlPanel`, `StatsPanel`, `SettingsDialog`, `HelpDialog`, `InspectionWizard`, `Theme.h` | ✅ |
| `utils/` | `Logger.h/.cpp`, `GpuUtils.h/.cpp`, `ImageUtils.h/.cpp` | ✅ |
| `ai/` | `InferenceEngine`, `ModelManager`, `ComponentDetector`, `OCREngine`, `SolderInspector` | ❌ non instancié |
| `features/` | `PickAndPlace`, `VoiceControl`, `BarcodeScanner`, `Measurement`, `StencilAlign`, `SnapshotHistory`, `RemoteView` | ❌ non instancié |
| `export/` | `ReportGenerator`, `DataExporter` | ❌ non instancié |

> `ai/`, `features/`, `export/` : code complet mais aucun `.onnx` dans `models/` — non instancié dans `Application`.

---

## Structures de données clés (`src/ibom/IBomData.h`)

```cpp
struct BBox   { double minX, minY, maxX, maxY; };       // PAS width/height
struct Point2D { double x, y; };                         // coords iBOM

struct Pad {
    std::string netName, pinNumber;
    Point2D position;
    enum Shape { Rect, RoundRect, Circle, Oval, Trapezoid, Custom };
    double sizeX, sizeY;
    bool isPin1, isSMD;
};

struct Component {
    std::string reference, value, footprint;
    enum class Layer { Front, Back };                    // ENUM CLASS, pas int
    Point2D position;  double rotation;  BBox bbox;
    std::vector<Pad> pads;
    std::vector<DrawingSegment> drawings;
    std::map<std::string, std::string> extraFields;
    std::map<std::string, bool> checkboxes;
};

struct BoardInfo {
    BBox boardBBox;                                      // PAS width/height
    std::string title, revision, date, company;
};
```

---

## Configuration (`src/app/Config.h`)

Defaults à connaître :

| Paramètre | Défaut | Notes |
|-----------|--------|-------|
| `cameraIndex` | 0 | Index DirectShow/V4L2 |
| `cameraWidth/Height` | 1920×1080 | |
| `cameraFps` | 30 | |
| `overlayOpacity` | 0.7 | |
| `showPads` | true | |
| `showSilkscreen` | true | |
| `showFabrication` | false | |
| `trackingIntervalMs` | 200 | Throttle entre appels ORB |
| `orbKeypoints` | 200 | Suffisant à 0.5× downscale |
| `minMatchCount` | 8 | Matches minimaux pour RANSAC |
| `matchDistanceRatio` | 0.75 | Lowe's ratio (0.5–0.95) |
| `ransacThreshold` | 3.0 | pixels |
| `trackingDownscale` | 0.5 | 0.1–1.0; 0.5 = ×4 moins de pixels |
| `calibBoardCols/Rows` | 7×5 | Inner corners checkerboard |
| `calibSquareSize` | 5.0 mm | |
| `scaleMethod` | Homography | Enum: None/Homography/IBomPads |
| `opticalMultiplier` | 1.0 | Adaptateur optique 0.5×–2× |
| `checkboxColumns` | ["Sourced","Placed"] | Colonnes BOM |

---

## Features fonctionnelles

- Caméra USB (MSMF/V4L2, 1920×1080@30fps), sélecteur de device
- iBOM parsing (JSON direct + LZ-String compressé) → overlay pads/silkscreen/labels + BOM panel
- Calibration caméra (checkerboard configurable, PDF patron intégré)
- Homographie manuelle (4 points) + live tracking (ORB + RANSAC, thread dédié)
- Dynamic scale px/mm (depuis homographie ou pads iBOM) + adaptateur optique (0.5×–2×)
- Settings dialog `Ctrl+,` (4 onglets, sauvegarde JSON)
- Overlay toggles (Pads / Silkscreen / Fabrication), opacity slider
- Camera fullscreen (double-clic → plein écran, Escape retour)
- Dark/Light theme (`Theme.h` centralise toutes les couleurs — Catppuccin Mocha/Latte)
- Help dialog (8 onglets), alignement 2 points
- ORB tracking (TrackingWorker) : Lowe's ratio test, downscale configurable, timing spdlog::debug

---

## Non connecté (sources existent, pas instancié)

`ai/InferenceEngine`, `ai/ComponentDetector`, `ai/OCREngine`, `ai/SolderInspector`,
`features/PickAndPlace`, `features/Measurement`, `features/RemoteView`,
`features/VoiceControl`, `features/BarcodeScanner`, `features/SnapshotHistory`,
`features/StencilAlign`, `export/ReportGenerator`, `export/DataExporter`

> `models/` vide — les modèles ONNX sont à entraîner (YOLOv8/RT-DETR) et exporter.

---

## Tests (`tests/`)

Framework : Catch2 3.5+

| Fichier | Couvre |
|---------|--------|
| `test_ibom_parser.cpp` | Parsing HTML/JSON, structures données |
| `test_homography.cpp` | Identité, translation, inverse |
| `test_inference.cpp` | Init ONNX Runtime, modèles vides |
| `test_component_matching.cpp` | ComponentMap, requêtes spatiales |

Run : `cd build && ctest --output-on-failure`

---

## Vcpkg — portfile patches (onnxruntime CUDA+TRT)

Ces patches sont dans le portfile buildtrees vcpkg. Si `vcpkg_installed` est supprimé et onnxruntime est recompilé, les réappliquer :

```
C:\Tools\vcpkg\buildtrees\versioning_\versions\onnxruntime\7391737791bbf21b648ab4e95521f31fdf502d59\portfile.cmake
```

| Patch | Contenu |
|-------|---------|
| A — CUDA archs | `-DCMAKE_CUDA_ARCHITECTURES=89;100;120` (Ada/Hopper/Blackwell) |
| B — CUDA flags | `-D__NV_NO_VECTOR_DEPRECATION_DIAG -Xcompiler /Zc:preprocessor` |
| C — TensorRT path | `set(TENSORRT_HOME "C:/TensorRT/TensorRT-10.15.1.29")` (hardcodé) |
| D — CXX flags | `/D__NV_NO_VECTOR_DEPRECATION_DIAG /Zc:preprocessor /EHsc /utf-8` |
| E — ORT_DEF2STR_HELPER | `L#x` → `L"" #x` (fix C2146 avec /Zc:preprocessor) |

Protobuf patch (dans `vcpkg_installed`, se perd si vcpkg réinstalle) :
```
build\vcpkg_installed\x64-windows\include\google\protobuf\message_lite.h
```
`struct Undefined;` → `struct Undefined {};`

---

## Système

| | |
|-|-|
| GPU | NVIDIA RTX 5070 Laptop (Blackwell sm_120, 8150 MB) |
| CUDA | 13.2 |
| cuDNN | 9.20.0 |
| TensorRT | 10.15.1.29 |
| Qt | 6.8.2 msvc2022_64 |
| OpenCV | 4.12 (vcpkg) |
| ONNX Runtime | 1.23.2 (CUDA + TensorRT EP) |
| CMake | 4.2.3 — NMake Makefiles uniquement |
| Compiler | MSVC 14.44 (VS Build Tools 2022 v17.14) |
| OS | Windows 11, user `bambo` |
