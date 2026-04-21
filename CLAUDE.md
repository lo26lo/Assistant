# MicroscopeIBOM — CLAUDE.md

## Pièges critiques

1. **vcvarsall.bat** → `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\...` (PAS `Community`)
2. **NE PAS supprimer** `build\vcpkg_installed\` — onnxruntime CUDA+TRT a pris ~2h à compiler
3. **NE PAS utiliser** `spdlog::flush_every(seconds(0))` → crash ACCESS_VIOLATION
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
14. **frameReady** : émettre `frame.clone()` PAS `frame` — évite dangling ref cross-thread. (Depuis avril 2026 : `FrameRef = shared_ptr<const cv::Mat>` zero-copy.)
15. **BFMatcher crossCheck** : pour `knnMatch(k=2)` il faut `crossCheck=false` — sinon l'appel échoue. Avec `crossCheck=true`, n'utiliser que `match()` simple.
16. **`matchDistanceRatio`** : sémantique changée en avril 2026 — c'est désormais le **Lowe's ratio** (0.5–0.95, typique 0.75), plus un multiplicateur de distance. Migration auto dans `Config::load()` si valeur ≥ 1.0.

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

## Architecture

```
main.cpp
  └─ QApplication
  └─ Application (QObject)
       ├─ Config (JSON, AppData)
       ├─ CameraCapture (thread séparé → frameReady → CameraView)
       ├─ CameraCalibration (YAML, undistort)
       ├─ IBomParser (HTML → JSON → IBomProject, supporte LZ-String)
       ├─ OverlayRenderer (pads + silkscreen + labels sur QImage)
       ├─ Homography (pcbToImage, transformRect, setMatrix)
       └─ MainWindow
            ├─ CameraView (paintEvent, overlay opacity, double-clic fullscreen)
            ├─ ControlPanel
            ├─ BomPanel (QTableWidget)
            ├─ StatsPanel (FPS, GPU, scale px/mm)
            ├─ SettingsDialog (4 onglets: Camera/Overlay/Tracking/AI)
            └─ InspectionWizard
```

## Features fonctionnelles

- Caméra USB (MSMF, 1920×1080@30fps)
- iBOM parsing (JSON direct + LZ-String compressé) → overlay pads/silkscreen/labels + BOM panel
- Calibration caméra (checkerboard configurable, PDF patron intégré)
- Homographie manuelle (4 points) + live tracking (ORB + RANSAC)
- Dynamic scale px/mm (depuis homographie ou pads iBOM) + adaptateur optique (0.5×–2×)
- Settings dialog Ctrl+, (4 onglets, sauvegarde JSON)
- Overlay toggles (Pads / Silkscreen / Fabrication), opacity slider
- Camera fullscreen (double-clic), camera selector
- Dark/Light theme (Theme.h centralise toutes les couleurs)
- Help dialog (8 onglets), alignement 2 points
- ORB tracking sur thread dédié (TrackingWorker) : Lowe's ratio test, downscale configurable [0.1, 1.0], timing spdlog::debug

## Non connecté (sources existent, pas instancié)

`ai/InferenceEngine`, `ai/ComponentDetector`, `ai/OCREngine`, `ai/SolderInspector`,
`features/PickAndPlace`, `features/Measurement`, `features/RemoteView`,
`features/VoiceControl`, `features/BarcodeScanner`, `features/SnapshotHistory`,
`features/StencilAlign`, `export/ReportGenerator`, `export/DataExporter`

> Models ONNX absents — `models/` vide.

## Système

| | |
|-|-|
| GPU | NVIDIA RTX 5070 Laptop (Blackwell sm_120, 8150 MB) |
| CUDA | 13.2 |
| TensorRT | 10.15.1.29 |
| Qt | 6.8.2 msvc2022_64 |
| OpenCV | 4.12 (vcpkg) |
| ONNX Runtime | 1.23.2 (CUDA + TensorRT EP) |
| CMake | 4.2.3 — NMake Makefiles uniquement |
| Compiler | MSVC 14.44 (VS Build Tools 2022 v17.14) |
