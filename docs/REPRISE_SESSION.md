# 🔄 Fichier de Reprise de Session — MicroscopeIBOM

> Mis à jour le 20 mars 2026

---

## 📊 État actuel

| Composant | État |
|-----------|------|
| build_windows.bat | ✅ Tous les bugs corrigés |
| CMake 4.2.3 | ✅ Installé (winget) |
| Générateur CMake | ✅ **NMake Makefiles** (Ninja a un bug rules.ninja) |
| VS Build Tools 2022 v17.14 | ✅ Installé |
| vcpkg | ✅ Installé et non-shallow |
| Qt 6.8.2 msvc2022_64 | ✅ Installé |
| CUDA 13.2 | ✅ Installé |
| cuDNN 9.20.0 | ✅ Installé + copié dans CUDA dir |
| TensorRT 10.15.1.29 | ✅ Extrait + détecté par CMake |
| **Tous packages vcpkg** | ✅ **INSTALLÉS** |
| **CMake configure projet** | ✅ **RÉUSSI** (NMake Makefiles) |
| **MicroscopeIBOM.exe** | ✅ **COMPILÉ ET LINKÉ** |
| **Tests (4/4)** | ✅ **100% PASSÉS** |
| **Exécution** | ✅ **App stable** |

### Travail session 20 mars 2026

| Tâche | État |
|-------|------|
| Settings dialog (4 onglets Camera/Overlay/Tracking/AI) | ✅ |
| Config tracking params (ORB, throttle, RANSAC, min matches) | ✅ |
| Camera fullscreen (double-clic + Escape) | ✅ |
| Overlay toggles fix (Show Pads/Silk/Fab → rendu conditionnel) | ✅ |
| showFabricationChanged wiring (signal était non connecté) | ✅ |
| Hardcoded values → Config reads dans Application.cpp | ✅ |
| settingsChanged signal → recreate ORB detector | ✅ |

### Fichiers modifiés session 20 mars

| Fichier | Modifications |
|---------|---------------|
| `src/app/Config.h` | +5 params tracking (trackingIntervalMs, orbKeypoints, minMatchCount, matchDistanceRatio, ransacThreshold) |
| `src/app/Config.cpp` | Load/save section "tracking" JSON |
| `src/gui/SettingsDialog.h` | NOUVEAU — QDialog 4 onglets |
| `src/gui/SettingsDialog.cpp` | NOUVEAU — formulaires, load/save Config |
| `src/gui/MainWindow.h` | +settingsChanged signal, +m_cameraFullscreen, +toggleCameraFullscreen() |
| `src/gui/MainWindow.cpp` | onShowSettings() impl, toggleCameraFullscreen(), double-clic connect |
| `src/gui/CameraView.h` | +doubleClicked signal, +mouseDoubleClickEvent |
| `src/gui/CameraView.cpp` | mouseDoubleClickEvent impl |
| `src/app/Application.cpp` | Config reads, settingsChanged handler, overlay toggles conditionnels, showFabricationChanged |
| `CMakeLists.txt` | +SettingsDialog.h/.cpp |
| `src/gui/StatsPanel.cpp` | Margins améliorés |

---

## 🗂️ Chemins importants

```
Projet          : C:\Users\bambo\Assistant\Assistant
vcpkg           : C:\Tools\vcpkg
Qt              : C:\Qt\6.8.2\msvc2022_64
CUDA            : C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
cuDNN           : C:\Program Files\NVIDIA\CUDNN\v9.20
TensorRT        : C:\TensorRT\TensorRT-10.15.1.29
VS Build Tools  : C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
CMake           : C:\Program Files\CMake\bin  (v4.2.3)
Python          : C:\Data\Python312\python.exe
```

---

## 🔧 Fichiers modifiés (CRITIQUES — ne pas écraser)

### 1. `C:\Tools\vcpkg\triplets\x64-windows.cmake`
Etat : **CLEAN** (stock vcpkg, aucun patch)
```cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
```

### 2. Portfile onnxruntime
`C:\Tools\vcpkg\buildtrees\versioning_\versions\onnxruntime\7391737791bbf21b648ab4e95521f31fdf502d59\portfile.cmake`

**Patch A** — Architecture CUDA :
```cmake
"-DCMAKE_CUDA_ARCHITECTURES=89;100;120"
```

**Patch B** — CMAKE_CUDA_FLAGS (fix longlong4 + CCCL) :
```cmake
"-DCMAKE_CUDA_FLAGS=-D__NV_NO_VECTOR_DEPRECATION_DIAG -Xcudafe --diag_suppress=2803 --diag-suppress=20199 -Wno-deprecated-gpu-targets -Xcompiler /Zc:preprocessor"
```

**Patch C** — TensorRT hardcodé :
```cmake
set(TENSORRT_HOME "C:/TensorRT/TensorRT-10.15.1.29")
```

**Patch D** — CMAKE_CXX_FLAGS :
```cmake
"-DCMAKE_CXX_FLAGS=/D__NV_NO_VECTOR_DEPRECATION_DIAG /Zc:preprocessor /EHsc /utf-8"
```

**Patch E** — `vcpkg_replace_string` pour le bug `ORT_DEF2STR_HELPER` (C2146 avec /Zc:preprocessor) :
```cmake
vcpkg_replace_string("${SOURCE_PATH}/onnxruntime/core/providers/tensorrt/tensorrt_execution_provider_custom_ops.cc"
    "#define ORT_DEF2STR_HELPER(x) L#x"
    "#define ORT_DEF2STR_HELPER(x) L\"\" #x"
)
```

### 3. Header protobuf patché (installé)
`C:\Users\bambo\Assistant\Assistant\build\vcpkg_installed\x64-windows\include\google\protobuf\message_lite.h`
- `struct Undefined;` → `struct Undefined {};`  (fix nvcc 13.2 type incomplet)
- **ATTENTION** : ce patch est dans `vcpkg_installed`, pas dans buildtrees. Si vcpkg réinstalle protobuf, il faut le réappliquer.

---

## ▶️ Commande de build (reprendre où on s'est arrêté)

### Configure (déjà fait — ne refaire que si build/ est nettoyé)
```cmd
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set TENSORRT_HOME=C:\TensorRT\TensorRT-10.15.1.29
set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
"C:\Program Files\CMake\bin\cmake.exe" -B build -S . -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=C:/Tools/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DIBOM_ENABLE_TENSORRT=ON -DQt6_DIR=C:/Qt/6.8.2/msvc2022_64/lib/cmake/Qt6
```

### Compile
```cmd
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set TENSORRT_HOME=C:\TensorRT\TensorRT-10.15.1.29
set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
cd /d C:\Users\bambo\Assistant\Assistant\build
nmake
```

> ⚠️ **IMPORTANT** : On utilise NMake Makefiles au lieu de Ninja (bug CMake 4.2.3). La compilation est mono-thread par défaut. Pour paralléliser, ajouter `CMAKE_BUILD_PARALLEL_LEVEL=8` ou utiliser `cmake --build build -j 8`.

---

## ✅ Erreur 18 résolue : CMake 4.2.3 + Ninja — rules.ninja introuvable

**Solution :** Utiliser `-G "NMake Makefiles"` au lieu de `-G Ninja`. Bug confirmé dans CMake 4.2.3 uniquement avec le générateur Ninja (try_compile ne génère pas `CMakeFiles/rules.ninja`).

## ✅ Erreurs 19-20 résolues : CMakeLists.txt du projet

- **Erreur 19** : `CompilerFlags.cmake` déplacé après `qt_add_executable()`
- **Erreur 20** : `FindTensorRT.cmake` mis à jour avec `$ENV{TENSORRT_HOME}` et noms versionnés (`nvinfer_10`)

---

## 📦 Dépendances vcpkg (vcpkg.json actuel)

```json
{
  "name": "microscope-ibom",
  "version": "0.1.0",
  "dependencies": [
    {"name": "opencv4", "default-features": false, "features": ["dnn", "highgui", "calib3d"]},
    "nlohmann-json",
    "spdlog",
    "catch2",
    {"name": "onnxruntime-gpu", "platform": "x64"},
    "cpr",
    "nu-book-zxing-cpp",
    "libharu"
  ],
  "builtin-baseline": "717f2c1db552beed995736e41accdf4f6f4ca986"
}
```

---

## 🖥️ Informations système

- **OS** : Windows 11, utilisateur `bambo`
- **GPU** : NVIDIA RTX 5070 (Blackwell, sm_120)
- **CUDA architectures ciblées** : sm_89 (Ada), sm_100 (Hopper), sm_120 (Blackwell)
- **vcpkg triplet** : x64-windows
- **vcpkg baseline SHA** : `717f2c1db552beed995736e41accdf4f6f4ca986`

---

## 🔧 Fichiers modifiés dans le projet (cette session)

### `CMakeLists.txt`
- Ajout de `set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)` avant `project()` (tenté pour erreur 18, peut être retiré)
- `include(CompilerFlags)` déplacé après `qt_add_executable()` (erreur 19)

### `cmake/FindTensorRT.cmake`
- Ajout de `$ENV{TENSORRT_HOME}` et `C:/TensorRT/TensorRT-10.15.1.29` dans les chemins de recherche
- Ajout des noms versionnés : `nvinfer_10`, `nvinfer_plugin_10`, `nvonnxparser_10`

### `cmake/CompilerFlags.cmake`
- Aucun changement de contenu, mais l'include est maintenant après la target

### Fixes compilation (session 19 mars 2026)
- `src/main.cpp` : Réécrit pour utiliser `Application(argc,argv).initialize().exec()`
- `src/app/Application.cpp` : `MainWindow(this)` au lieu de 3 args séparés
- `src/gui/BomPanel.cpp` : `Layer::Front` au lieu de `== 0`
- `src/gui/InspectionWizard.h` : Ajout `#include <QTableWidget>`
- `src/features/PickAndPlace.h` : Type `Layer` au lieu de `int` + include IBomData.h
- `src/features/BarcodeScanner.cpp` : Ajout `#include <opencv2/imgproc.hpp>`
- `src/features/SnapshotHistory.cpp` : Ajout `#include <QRegularExpression>`
- `src/export/DataExporter.h/.cpp` : Type `Layer`, `#include <QDateTime>`, méthodes export non-const
- `CMakeLists.txt` : `unofficial-libharu` pour le find_package/link
- `tests/CMakeLists.txt` : Chaque test compile son source correspondant + libs
- `tests/test_*.cpp` (4 fichiers) : APIs mises à jour (Layer enum, signatures, constructeurs)

---

## 🧹 Si tout est cassé : commandes de nettoyage

```powershell
# 1. Tuer les processus bloquants
Get-Process cmd, nvcc, cmake -ErrorAction SilentlyContinue | Stop-Process -Force

# 2. Nettoyer uniquement le build projet (PAS vcpkg_installed !)
Remove-Item -Recurse -Force "C:\Users\bambo\Assistant\Assistant\build\CMakeFiles"
Remove-Item -Force "C:\Users\bambo\Assistant\Assistant\build\CMakeCache.txt"
Remove-Item -Force "C:\Users\bambo\Assistant\Assistant\build\Makefile"
```

> ⚠️ **NE PAS** supprimer `build\vcpkg_installed` — tous les packages sont déjà compilés (notamment onnxruntime CUDA+TRT qui prend ~2h).

---

## 🚨 Erreurs résolues cette session

- **Erreur 32** : EXE verrouillé (LNK1104) → `Stop-Process`
- **Erreur 33** : GUI moche + caméra non fonctionnelle → Stylesheet + cv::Mat metatype (en validation)
- **Erreur 34** : Mauvais chemin vcvarsall.bat → BuildTools au lieu de Community
- **Erreur 35** : ACCESS_VIOLATION crash → flush_every(0) + atomic non-safe dans FPS timer

### Commande de rebuild rapide (après modification sources seulement)
```cmd
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
set TENSORRT_HOME=C:\TensorRT\TensorRT-10.15.1.29
set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
cd /d C:\Users\bambo\Assistant\Assistant\build
nmake
```

### Lancement et vérification
```powershell
cd C:\Users\bambo\Assistant\Assistant\build\bin
.\MicroscopeIBOM.exe
# Si crash, vérifier les logs :
Get-Content logs\pcb_inspector.log -Tail 30
```
