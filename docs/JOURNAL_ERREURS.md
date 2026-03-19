# 🐛 Journal des Erreurs — MicroscopeIBOM

> Sessions du 18-19 mars 2026  
> GPU : NVIDIA RTX 5070 | CUDA 13.2 | TensorRT 10.15.1 | onnxruntime 1.23.2  
> **Build :** ✅ RÉUSSI + 4/4 TESTS OK  
> **Exécution :** ✅ App stable (crash ACCESS_VIOLATION résolu)

---

## ERREUR 1 — `[0m était inattendu.` (build_windows.bat plante au démarrage)

**Symptôme :**
```
[0m était inattendu.
```
Le script `build_windows.bat` s'arrête immédiatement.

**Cause :**
Les codes ANSI de couleur dans les `echo` contenaient des parenthèses `()`.  
Sous `cmd.exe` avec `EnableDelayedExpansion`, les `(` et `)` dans une chaîne echo à l'intérieur d'un bloc `if` sont interprétés comme des délimiteurs de bloc.

**Ce qui n'a PAS fonctionné :**
- Rien d'autre n'a été tenté.

**Correction appliquée ✅ :**
Remplacer tous les `(` par `^(` et tous les `)` par `^)` dans les instructions `echo` à l'intérieur des blocs `if`.  
Exemple :
```bat
:: Avant (cassé)
echo   [OK] Qt6 detecte (version %QT_VERSION%)
:: Après (corrigé)
echo   [OK] Qt6 detecte ^(version %QT_VERSION%^)
```
11 occurrences corrigées dans `build_windows.bat`.

---

## ERREUR 2 — Les marqueurs `[!!]` disparaissent dans le script

**Symptôme :**
Les marqueurs `[!!]` dans les messages d'erreur s'affichaient vides ou incorrects.

**Cause :**
`EnableDelayedExpansion` consomme les `!` — `[!!]` devient `[]`.

**Correction appliquée ✅ :**
Remplacer `[!!]` par `[XX]` dans tous les messages d'affichage.

---

## ERREUR 3 — La commande `tee` est inconnue

**Symptôme :**
```
'tee' n'est pas reconnu comme une commande interne ou externe
```

**Cause :**
`tee` est une commande Unix qui n'existe pas nativement sous Windows.

**Correction appliquée ✅ :**
Supprimer le pipe vers `tee` dans le script.

---

## ERREUR 4 — Ninja introuvable après `vcvarsall.bat`

**Symptôme :**
CMake ne trouve pas Ninja comme générateur après que `vcvarsall.bat` ait modifié le PATH.

**Cause :**
`vcvarsall.bat` réinitialise le PATH, écrasant le chemin vers Ninja ajouté manuellement.

**Ce qui n'a PAS fonctionné :**
- Ajouter Ninja au PATH avant l'appel à vcvarsall.

**Correction appliquée ✅ :**
Détecter explicitement le chemin de Ninja et le passer à CMake via `-DCMAKE_MAKE_PROGRAM=` :
```bat
for /f "tokens=*" %%i in ('where ninja 2^>nul') do set NINJA_PATH=%%i
cmake ... -DCMAKE_MAKE_PROGRAM="%NINJA_PATH%"
```

---

## ERREUR 5 — Baseline vcpkg invalide

**Symptôme :**
```
error: while loading the manifest: Failed to find the baseline
```

**Cause :**
Le champ `builtin-baseline` dans `vcpkg.json` contenait une date (`"2024.09.30"`) au lieu d'un SHA de commit Git.

De plus, vcpkg était un clone superficiel (`shallow clone`) et ne pouvait pas résoudre les commits anciens.

**Ce qui n'a PAS fonctionné :**
- Laisser la date telle quelle.
- Tenter de résoudre sans unshallow.

**Corrections appliquées ✅ :**
1. Unshallow le clone vcpkg :
   ```powershell
   cd C:\Tools\vcpkg; git fetch --unshallow
   ```
2. Remplacer la date par le SHA réel de la HEAD de vcpkg :
   ```json
   "builtin-baseline": "717f2c1db552beed995736e41accdf4f6f4ca986"
   ```

---

## ERREUR 6 — Port `zxing-cpp` introuvable

**Symptôme :**
```
error: no port named 'zxing-cpp'
```

**Cause :**
Le nom du port dans vcpkg pour cette version est `nu-book-zxing-cpp`, pas `zxing-cpp`.

**Correction appliquée ✅ :**
Dans `vcpkg.json` :
```json
// Avant
"zxing-cpp"
// Après
"nu-book-zxing-cpp"
```

---

## ERREUR 7 — Features opencv4 invalides

**Symptôme :**
```
error: the feature 'features2d' is not supported
error: the feature 'imgproc' is not supported
```

**Cause :**
Les features `core`, `features2d`, `imgproc`, `videoio` ne sont pas des features séparées dans opencv4 4.12 — elles font partie des features par défaut.

**Correction appliquée ✅ :**
```json
// Avant
{"name": "opencv4", "features": ["core", "features2d", "imgproc", "videoio", "dnn", "highgui", "calib3d"]}
// Après
{"name": "opencv4", "default-features": false, "features": ["dnn", "highgui", "calib3d"]}
```

---

## ERREUR 8 — CUDA absent du PATH (cmd.exe)

**Symptôme :**
```
'nvcc' n'est pas reconnu comme une commande interne ou externe
```
CUDA était installé mais `nvcc.exe` n'était pas dans le PATH de cmd.exe (seulement dans les nouvelles sessions après installation).

**Correction appliquée ✅ :**
Ajouter explicitement le chemin dans la commande de lancement :
```cmd
set PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\bin;%PATH%
```

---

## ERREUR 9 — cuDNN absent

**Symptôme :**
```
Could not find cudnn.h
```

**Cause :**
cuDNN n'était pas installé. onnxruntime-gpu en a besoin pour la compilation.

**Correction appliquée ✅ :**
1. Téléchargement de cuDNN 9.20.0 depuis NVIDIA
2. Extrait à `C:\Program Files\NVIDIA\CUDNN\v9.20\`
3. Copié les fichiers dans le répertoire CUDA (nécessitait droits admin) :
   ```powershell
   Copy-Item "C:\Program Files\NVIDIA\CUDNN\v9.20\include\12.9\cudnn*.h" "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\include\"
   Copy-Item "C:\Program Files\NVIDIA\CUDNN\v9.20\lib\12.9\x64\cudnn*.lib" "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\lib\x64\"
   ```

---

## ERREUR 10 — Architecture GPU non supportée par CUDA 13.2

**Symptôme :**
```
nvcc fatal: Unsupported gpu architecture 'compute_60'
```

**Cause :**
CUDA 13.2 a supprimé le support des architectures anciennes (compute_60, compute_61...). Le portfile onnxruntime avait `# "-DCMAKE_CUDA_ARCHITECTURES=native"` commenté, et utilisait donc les architectures par défaut qui incluaient des valeurs obsolètes.

**Ce qui n'a PAS fonctionné :**
- Définir la variable d'environnement `CUDAARCHS=89` avant le build (vcpkg ne la propage pas).

**Correction appliquée ✅ :**
Modifier directement le portfile (`portfile.cmake` ligne 75) :
```cmake
// Avant
# "-DCMAKE_CUDA_ARCHITECTURES=native"
// Après
"-DCMAKE_CUDA_ARCHITECTURES=89;100;120"
```
- `89` = Ada Lovelace (RTX 4xxx)
- `100` = Hopper (H100)
- `120` = Blackwell (RTX 5xxx — notre GPU)

---

## ERREUR 11 — TensorRT non trouvé

**Symptôme :**
```
Can't find NV_TENSORRT_MAJOR macro
Can't find nvinfer
Can't find nvonnxparser
TENSORRT_LIBRARY_INFER-NOTFOUND
```

**Cause :**
TensorRT n'était pas installé. `onnxruntime-gpu` dans vcpkg active automatiquement les features `cuda` ET `tensorrt`.

**Ce qui n'a PAS fonctionné :**
- Définir la variable d'environnement `TENSORRT_HOME` avant le build (vcpkg ne la propage pas dans les portfiles).

**Correction appliquée ✅ :**
1. Télécharger TensorRT 10.15.1 GA (ZIP CUDA 13.0-13.1) depuis NVIDIA, extraire à `C:\TensorRT\TensorRT-10.15.1.29`
2. **Hardcoder** le chemin directement dans le portfile (ligne ~88) :
   ```cmake
   set(TENSORRT_HOME "C:/TensorRT/TensorRT-10.15.1.29")
   ```

---

## ERREUR 12 — Préprocesseur MSVC non conforme (CUDA 13.2 + CCCL)

**Symptôme :**
```
fatal error C1189: #error: MSVC/cl.exe with traditional preprocessor is used.
Please switch to the standard conforming preprocessor by passing /Zc:preprocessor to cl.exe.
```

**Cause :**
Les headers CCCL (CUDA C++ Core Libraries) inclus dans CUDA 13.2 exigent le préprocesseur conforme de MSVC (`/Zc:preprocessor`). onnxruntime ne le passait pas à cl.exe via nvcc.

**Ce qui n'a PAS fonctionné :**
- Rien d'autre n'a été tenté.

**Correction appliquée ✅ :**
Ajouter `-Xcompiler /Zc:preprocessor` aux `CMAKE_CUDA_FLAGS` dans le portfile :
```cmake
"-DCMAKE_CUDA_FLAGS=... -Xcompiler /Zc:preprocessor"
```
Et ajouter `/Zc:preprocessor` aux `CMAKE_CXX_FLAGS` pour les fichiers `.cc` compilés directement par cl.exe :
```cmake
"-DCMAKE_CXX_FLAGS=/Zc:preprocessor /EHsc /utf-8"
```

---

## ERREUR 13 — `longlong4` déprécié dans CUDA 13.2 (C4996, le plus difficile)

**Symptôme :**
```
error C4996: 'longlong4': use longlong4_16a or longlong4_32a
```
Environ 6 fichiers `.cu` échouent dans `contrib_ops/cuda/bert/`.

**Cause :**
CUDA 13.2 a déprécié le type `longlong4` (remplacé par `longlong4_16a` ou `longlong4_32a`). onnxruntime 1.23.2 utilise encore l'ancien type. Le flag `/sdl` (Security Development Lifecycle) dans la ligne de compilation nvcc **transforme les warnings C4996 en erreurs**, annulant les `/wd4996` ajoutés plus tôt.

**Ce qui n'a PAS fonctionné :**

1. ❌ **`/wd4996` dans `CMAKE_CUDA_FLAGS`** via `-Xcompiler /wd4996` :  
   `/sdl` vient après et réactive C4996 comme erreur.

2. ❌ **Force-include d'un header via `/FI`** dans `CMAKE_CUDA_FLAGS` :  
   `/FIC:/Tools/vcpkg/suppress_cuda13_compat.h` — le chemin n'était pas correctement transmis à nvcc pour les fichiers stub générés.

3. ❌ **`VCPKG_CXX_FLAGS "/wd4996"` dans le triplet seul** :  
   vcpkg gardait l'ancien ABI en cache malgré le changement de triplet (cache binaire `archives` non vidé).

4. ❌ **`vcpkg_replace_string` seul** (injecter `#pragma warning(disable:4996)` dans `attention_impl.h`) :  
   Le `#pragma` couvre le header mais pas les fichiers **stub `.c`** temporaires générés par nvcc (ex: `attention_strided_copy.cudafe1.stub.c`) qui incluent les types dépréciés indépendamment.

**Correction finale appliquée ✅ (combinaison de 4 mécanismes) :**

1. Triplet `x64-windows.cmake` — `/wd4996` global :
   ```cmake
   set(VCPKG_CXX_FLAGS "/wd4996")
   set(VCPKG_C_FLAGS "/wd4996")
   ```

2. `CMAKE_CXX_FLAGS` dans le portfile :
   ```cmake
   "-DCMAKE_CXX_FLAGS=/Zc:preprocessor /EHsc /utf-8 /wd4996"
   ```

3. `CMAKE_CUDA_FLAGS` dans le portfile (via `-Xcompiler`) :
   ```cmake
   "-DCMAKE_CUDA_FLAGS=... -Xcompiler /Zc:preprocessor -Xcompiler /wd4996"
   ```

4. `vcpkg_replace_string` dans le portfile (injection de pragma source) :
   ```cmake
   vcpkg_replace_string("${SOURCE_PATH}/onnxruntime/contrib_ops/cuda/bert/attention_impl.h"
       "#pragma once"
       "#pragma once\n#ifdef _MSC_VER\n#pragma warning(disable: 4996)\n#endif"
   )
   ```

5. **Vider le cache binaire vcpkg** pour forcer la prise en compte du nouveau triplet :
   ```powershell
   Remove-Item -Recurse -Force "C:\Users\bambo\AppData\Local\vcpkg\archives"
   ```

> **Leçon** : La combinaison de `/sdl` (qui transforme les warnings en erreurs) avec les fichiers stub intermédiaires de nvcc rend très difficile la suppression sélective de C4996. Il faut agir à TOUS les niveaux simultanément.

---

## ERREUR 14 — `CMAKE_CXX_FLAGS` causait un warning vcpkg "MAYBE_UNUSED_VARIABLES"

**Symptôme :**
```
CMake Warning: CMAKE_CXX_FLAGS passed but unused
```

**Cause :**
vcpkg signale les variables CMake passées via `OPTIONS` mais non déclarées dans `MAYBE_UNUSED_VARIABLES`.

**Correction appliquée ✅ :**
Ajouter `CMAKE_CUDA_FLAGS` et `CMAKE_CXX_FLAGS` à la liste `MAYBE_UNUSED_VARIABLES` dans `vcpkg_cmake_configure()` du portfile.

---

## ERREUR 15 — Build bloqué / fichier `build_output.txt` verrouillé

**Symptôme :**
Le fichier `build_output.txt` avait toujours le même contenu même après un nouveau lancement du build.

**Cause :**
Des processus `cmd.exe` zombies des tentatives de build précédentes avaient le fichier ouvert en écriture exclusive.

**Correction appliquée ✅ :**
1. Identifier les processus :
   ```powershell
   Get-Process cmd -ErrorAction SilentlyContinue | Select-Object Id, StartTime
   ```
2. Les tuer :
   ```powershell
   Stop-Process -Id <PID> -Force
   ```
3. Utiliser un **nouveau nom de fichier** pour le prochain build :
   ```cmd
   > build_log2.txt
   ```

---

---

## ERREUR 16 — `EnumTraitsImpl::Undefined` — type incomplet (nvcc 13.2)

**Symptôme :**
```
message_lite.h(298): error: variable cannot have incomplete type "google::protobuf::internal::EnumTraitsImpl::Undefined"
```
Atteint à `[532/557]` du build CUDA.

**Cause :**
nvcc 13.2 rejette les types incomplets dans les déclarations de variables template. `struct Undefined;` est une forward declaration acceptée par MSVC mais rejetée par nvcc 13.2 strict.  
La suppression `--diag-suppress=20199` ne fonctionne que pour les warnings, pas pour cette erreur dure.

**Ce qui n'a PAS fonctionné :**
- `--diag-suppress=20199` dans `CMAKE_CUDA_FLAGS` : supprime uniquement les warnings, pas les hard errors nvcc.

**Correction appliquée ✅ :**
Patch direct du header protobuf installé :
```
C:\Users\bambo\Assistant\Assistant\build\vcpkg_installed\x64-windows\include\google\protobuf\message_lite.h
```
Ligne ~298 : `struct Undefined;` → `struct Undefined {};`

> ⚠️ Ce patch est dans `vcpkg_installed`, pas dans buildtrees. Il sera perdu si vcpkg réinstalle protobuf.

---

## ERREUR 17 — `C2146: absence de ')' avant 'L'` dans tensorrt_execution_provider_custom_ops.cc

**Symptôme :**
```
tensorrt_execution_provider_custom_ops.cc(88): error C2146: erreur de syntaxe: absence de ')' avant 'L'
tensorrt_execution_provider_custom_ops.cc(88): error C2065: 'L': identificateur non déclaré
```

**Cause :**
Dans `tensorrt_execution_provider_custom_ops.cc` v1.23.2 :
```cpp
#define ORT_DEF2STR_HELPER(x) L#x
```
Avec `/Zc:preprocessor` (conforme C++20), `L#x` produit **deux tokens séparés** : `L` (identificateur) + `"10"` (string littérale). Le compilateur ne les fusionne pas en wide string.  
Bug documenté dans le `main` branch ORT (commit b5f869b).

**Correction appliquée ✅ (fix officiel upstream ORT main branch) :**
```cmake
vcpkg_replace_string("${SOURCE_PATH}/onnxruntime/core/providers/tensorrt/tensorrt_execution_provider_custom_ops.cc"
    "#define ORT_DEF2STR_HELPER(x) L#x"
    "#define ORT_DEF2STR_HELPER(x) L\"\" #x"
)
```
`L"" #x` produit `L""` (wide string token) + `"10"` (narrow string) → concaténation littérale standard = `L"10"` ✓  
Ajouté dans le portfile ET appliqué directement dans le source tree.

---

## ERREUR 18 — `rules.ninja: GetLastError() = 2` (CMake 4.2.3 + Ninja)

**Symptôme :**
```
ninja: error: build.ninja:35: loading 'CMakeFiles\rules.ninja': GetLastError() = 2
CMake Error at ...CMakeDetermineCompilerABI.cmake:83 (try_compile):
  Failed to generate test project build system.
```
Survient lors du `cmake -B build -S .` du projet MicroscopeIBOM, après que tous les packages vcpkg sont installés.

**Cause :**
Bug confirmé dans CMake 4.2.3 avec le générateur Ninja sur Windows. Lors du `try_compile` de détection ABI du compilateur CXX, CMake génère `build.ninja` qui inclut `CMakeFiles\rules.ninja`, mais ce dernier n'est **jamais créé** dans le sous-répertoire TryCompile. Reproduit même avec un projet minimal (2 fichiers), donc pas lié au projet, à vcpkg, ni à la locale.

**Diagnostic avec `--debug-trycompile` :**
- `build.ninja` : ✅ créé dans TryCompile
- `CMakeFiles/rules.ninja` : ❌ absent
- Pas de dossier `CMakeFiles/4.2.3/` (détection compilateur incomplète)
- vcpkg interne (detect_compiler) fonctionne avec Ninja — différence dans les flags du sous-cmake

**Ce qui n'a PAS fonctionné :**
1. ❌ `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` dans CMakeLists.txt
2. ❌ Ninja copié vers chemin sans espaces (`C:\Tools\ninja\ninja.exe`)
3. ❌ `CMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_MAKE_PROGRAM`
4. ❌ Downgrade CMake vers 4.1.1 (packages vcpkg invalides car hash ABI différent)
5. ❌ `set VSLANG=1033` (forcer locale anglaise — la locale n'est pas la cause)

**Correction appliquée ✅ :**
Remplacer le générateur Ninja par **NMake Makefiles** :
```
cmake -G "NMake Makefiles" -B build -S .
```
NMake Makefiles ne souffre pas du bug `rules.ninja` car il n'utilise pas le système de fichiers Ninja. Le `try_compile` fonctionne normalement.

---

## ERREUR 19 — `Cannot specify compile options for target "MicroscopeIBOM"`

**Symptôme :**
```
CMake Error at cmake/CompilerFlags.cmake:6 (target_compile_options):
  Cannot specify compile options for target "MicroscopeIBOM" which is not built by this project.
```

**Cause :**
`include(CompilerFlags)` était appelé à la ligne 102 du `CMakeLists.txt`, **avant** la création de l'exécutable `qt_add_executable(${PROJECT_NAME} ...)` à la ligne 229. `CompilerFlags.cmake` utilise `target_compile_options(${PROJECT_NAME} ...)` qui nécessite que la target existe déjà.

**Correction appliquée ✅ :**
Déplacé `include(CompilerFlags)` après les `target_link_libraries` et les options optionnelles (CUDA, libharu), juste avant les tests.

---

## ERREUR 20 — `Could NOT find TensorRT (missing: TensorRT_nvinfer)`

**Symptôme :**
```
-- Could NOT find TensorRT (missing: TensorRT_nvinfer)
```

**Cause :**
Deux problèmes dans `cmake/FindTensorRT.cmake` :
1. `$ENV{TENSORRT_HOME}` n'était pas dans la liste `_TRT_SEARCH_PATHS`
2. Les libs TensorRT 10.x sont nommées `nvinfer_10.lib`, `nvinfer_plugin_10.lib` etc. (avec suffixe version), et `find_library` ne cherchait que `nvinfer`

**Correction appliquée ✅ :**
```cmake
# Ajout de TENSORRT_HOME et du chemin hardcodé
set(_TRT_SEARCH_PATHS
    "$ENV{TENSORRT_HOME}"
    "C:/TensorRT/TensorRT-10.15.1.29"
    ...
)

# Noms versionnés
find_library(TensorRT_nvinfer NAMES nvinfer nvinfer_10 ...)
find_library(TensorRT_nvinfer_plugin NAMES nvinfer_plugin nvinfer_plugin_10 ...)
find_library(TensorRT_nvonnxparser NAMES nvonnxparser nvonnxparser_10 ...)
```

---

## Résumé chronologique

| # | Erreur | Cause racine | Corrigée ? |
|---|--------|-------------|------------|
| 1 | `[0m était inattendu` | `()` dans echo/if batch | ✅ |
| 2 | `[!!]` disparaissent | `!` consommé par DelayedExpansion | ✅ |
| 3 | `tee` introuvable | Commande Unix sur Windows | ✅ |
| 4 | Ninja perdu après vcvarsall | PATH réinitialisé | ✅ |
| 5 | baseline vcpkg invalide | Date au lieu de SHA Git | ✅ |
| 6 | Port `zxing-cpp` introuvable | Mauvais nom de port | ✅ |
| 7 | Features opencv4 invalides | Features inexistantes dans v4.12 | ✅ |
| 8 | CUDA absent du PATH | Session cmd sans variables NVIDIA | ✅ |
| 9 | cuDNN absent | Non installé | ✅ |
| 10 | Architecture GPU non supportée | compute_60 retiré de CUDA 13.2 | ✅ |
| 11 | TensorRT non trouvé | Non installé / env var non propagée | ✅ |
| 12 | `/Zc:preprocessor` manquant | CCCL exige préprocesseur conforme | ✅ |
| 13 | `longlong4` déprécié (C4996) | CUDA 13.2 + `/sdl` en erreur | ✅ (__NV_NO_VECTOR_DEPRECATION_DIAG) |
| 14 | Warning MAYBE_UNUSED_VARIABLES | Variables non déclarées | ✅ |
| 15 | Fichier build_output.txt verrouillé | Processus zombies | ✅ |
| 16 | `EnumTraitsImpl::Undefined` nvcc 13.2 | Forward decl rejetée par nvcc strict | ✅ (patch message_lite.h) |
| 17 | `C2146 L` dans tensorrt custom ops | `L#x` = 2 tokens avec /Zc:preprocessor | ✅ (L"" #x fix upstream ORT) |
| 18 | `rules.ninja: GetLastError() = 2` | Bug CMake 4.2.3 + Ninja try_compile | ✅ (NMake Makefiles) |
| 19 | `Cannot specify compile options` | CompilerFlags.cmake avant target | ✅ (déplacé include) |
| 20 | `TensorRT_nvinfer` introuvable | Noms libs versionnés + path manquant | ✅ (nvinfer_10 + TENSORRT_HOME) |
| 21 | Icons manquants (resources/icons/) | Seulement README.md dans le dossier | ✅ (9 PNG placeholder 32x32 créés) |
| 22 | `Application` default ctor absent | main.cpp appelait `Application()` | ✅ (réécrit : `Application(argc,argv)`) |
| 23 | `MainWindow` mauvais args ctor | 3 args au lieu de `Application*` | ✅ (`MainWindow(this)`) |
| 24 | `Layer` enum class vs int (C2676) | `comp.layer == 0` au lieu de `Layer::Front` | ✅ (BomPanel, PickAndPlace, DataExporter) |
| 25 | `QTableWidget` non défini (C2143) | Include manquant InspectionWizard.h | ✅ |
| 26 | `cv::cvtColor` non membre (C2039) | opencv2/imgproc.hpp manquant | ✅ (BarcodeScanner.cpp) |
| 27 | `QRegularExpression` non défini (C2027) | Include manquant SnapshotHistory.cpp | ✅ |
| 28 | `emit` depuis méthode const (C2662) | DataExporter exports étaient const | ✅ (const retiré + QDateTime include) |
| 29 | Linker HPDF_* non résolu (LNK2019) | `find_package(libharu)` → `unofficial-libharu` | ✅ |
| 30 | Tests API obsolètes | Tests écrits contre anciennes signatures | ✅ (4 fichiers test réécrits) |
| 31 | Tests LNK2019 (sources manquants) | Test .cpp compilé seul sans impl | ✅ (tests/CMakeLists.txt : sources + libs) |
| 32 | EXE verrouillé (LNK1104) | MicroscopeIBOM.exe en cours d'exécution | ✅ (Stop-Process) |
| 33 | GUI moche + caméra non fonctionnelle | cv::Mat non enregistré + style CSS pauvre | 🔄 (fix appliqué, à valider visuellement) |
| 34 | Mauvais chemin vcvarsall.bat | `Community` au lieu de `BuildTools` | ✅ |
| 35 | ACCESS_VIOLATION au démarrage (0xC0000005) | `spdlog::flush_every(seconds(0))` + atomic non-safe | ✅ |

---

## Session du 19 mars 2026 (après-midi) — Amélioration GUI + Exécution

---

## ERREUR 32 — LNK1104 : Cannot open file 'MicroscopeIBOM.exe'

**Symptôme :**
```
LINK : fatal error LNK1104: cannot open file '...\bin\MicroscopeIBOM.exe'
```

**Cause :**
L'exécutable était encore en cours d'exécution (explorateur/lancé manuellement).

**Correction appliquée ✅ :**
```powershell
Stop-Process -Name MicroscopeIBOM -Force -ErrorAction SilentlyContinue
```

---

## ERREUR 33 — GUI visuellement pauvre + Caméra non fonctionnelle

**Symptôme :**
L'application se lance mais :
1. L'interface est visuellement cramée (group boxes trop épais, layout serré, style plat)
2. Le bouton Start Camera ne produit aucune image

**Cause (caméra) :**
`cv::Mat` n'était pas enregistré comme Qt metatype. Le signal `frameReady(cv::Mat)` via `Qt::QueuedConnection` (cross-thread) était silencieusement ignoré car Qt ne savait pas sérialiser `cv::Mat` pour la queue d'événements.

De plus, le signal `cameraSettingsChanged` de `ControlPanel` n'était connecté nulle part.

**Cause (GUI) :**
Stylesheets CSS minimales, mauvais margins/padding, group boxes avec bordures épaisses.

**Corrections appliquées 🔄 :**

### Caméra :
- `Application.cpp` : Ajout `Q_DECLARE_METATYPE(cv::Mat)` + `qRegisterMetaType<cv::Mat>("cv::Mat")`
- `Application.cpp` : Connexion du signal `cameraSettingsChanged` vers le slot de reconfiguration caméra
- `Application.cpp` : Ajout FPS timer (`QTimer` 1s) pour tracking frame rate
- `Application.h` : Ajout `QTimer* m_fpsTimer` + `std::atomic<int> m_frameCount`

### GUI :
- `MainWindow.cpp` : Réécriture complète de `applyDarkStylesheet()` et `applyLightStylesheet()` — thème sombre #111118 avec accent #6488e8, group boxes borderless avec séparateur top, meilleur padding
- `ControlPanel.cpp` : Spacing 8px, margins 4-8-4-8 dans tous les groupes
- `CameraView.cpp` : Nouveau placeholder avec dégradé et icône caméra géométrique
- `StatsPanel.cpp` : Margins 8-6-8-6, spacing 8

> **Statut :** Non encore validé visuellement — l'app crash au démarrage (erreur 35)

---

## ERREUR 34 — Mauvais chemin vcvarsall.bat dans les commandes terminal

**Symptôme :**
```
Le chemin d'accès spécifié est introuvable.
```
OU : nmake sans output (silencieusement échoué).

**Cause :**
Utilisation de `C:\Program Files\Microsoft Visual Studio\2022\Community\...` au lieu du chemin correct :
```
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat
```
Visual Studio **Build Tools** est installé, pas l'édition Community.

**Correction appliquée ✅ :**
Utiliser le bon chemin tel que documenté dans REPRISE_SESSION.md.

---

## ERREUR 35 — ACCESS_VIOLATION au démarrage (0xC0000005)

**Symptôme :**
L'application se ferme immédiatement après lancement. Exit code : `-1073741819` = `0xC0000005` (ACCESS_VIOLATION / segfault).

Le log s'arrête après le GPU warmup — aucun message de `Application::initialize()` n'apparaît dans les logs.

**Diagnostic :**
1. Ajout de logging debug dans `createSubsystems()` — les messages spdlog de Application.cpp ne s'affichent pas dans le fichier log
2. Ajout de `spdlog::flush_every(std::chrono::seconds(0))` pour forcer le flush — c'est **la cause probable du crash**
3. `spdlog::flush_every(seconds(0))` crée un thread de flush avec un intervalle de 0 secondes → spin-loop ou comportement indéfini → crash
4. De plus, `setupLogging()` dans Application::initialize() appelait `Logger::initialize()` une 2e fois (déjà fait dans main.cpp), recréant le default logger

**Ce qui n'a PAS fonctionné :**
- Nettoyage du QSettings Windows Registry (le crash est avant la GUI)

**Correction finale appliquée ✅ (2 causes identifiées) :**

1. **`spdlog::flush_every(std::chrono::seconds(0))`** — crée un thread de flush avec intervalle 0 = spin-loop. Remplacé par `spdlog::flush_on(spdlog::level::info)`. Supprimé le double appel `setupLogging()` (déjà fait dans main.cpp).

2. **FPS timer callback non-safe** — `m_frameCount = 0` sur un `std::atomic<int>` avec opérateur implicite, et accès potentiellement invalide au statsPanel. Remplacé par `m_frameCount.exchange(0)` + null-checks sur les pointeurs.

3. **Crash handler ajouté** dans `main.cpp` — `SetUnhandledExceptionFilter` pour logger l'adresse du crash en cas de récidive.

> **Résultat : App stable après 12+ secondes. ✅**
