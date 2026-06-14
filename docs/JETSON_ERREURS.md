# Journal des Erreurs — Migration Jetson AGX Orin

> **But du document** : recenser chaque erreur rencontrée pendant la migration Jetson, sa cause et sa solution. Permet d'éviter de re-débugger les mêmes problèmes.
>
> **Convention** : entrées numérotées dans l'ordre chronologique. Une entrée par symptôme distinct.
>
> **Documents liés** :
> - [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) — chronologie des sessions
> - [JETSON_MIGRATION.md](JETSON_MIGRATION.md) — plan global
> - [JOURNAL_ERREURS.md](JOURNAL_ERREURS.md) — anciennes erreurs Windows (référence)

---

## Index

| # | Date | Composant | Statut | Titre court |
|---|------|-----------|--------|-------------|
| 17 | 2026-06-14 | Application.cpp / caméra | 🟡 CONTOURNÉ | [`Found 0 camera(s)` sur device V4L2 fonctionnel — énumération via QMediaDevices au lieu d'OpenCV](#erreur-17--found-0-cameras-sur-device-v4l2-fonctionnel--enumeration-via-qmediadevices) |
| 16 | 2026-06-14 | CMakeLists.txt / libharu | ✅ RÉSOLU | [Link `undefined reference HPDF_*` — header `<hpdf.h>` présent mais lib non linkée](#erreur-16--link-undefined-reference-hpdf_--header-présent-mais-lib-non-linkée) |
| 15 | 2026-06-10 | compose.local.yml / camera | ✅ RÉSOLU | [Caméra USB vue par lsusb mais "No camera detected" dans l'app — /dev/video* non mappés](#erreur-15--caméra-usb-vue-par-lsusb-mais-no-camera-detected-dans-lapp--devvideo-non-mappés) |
| 14 | 2026-06-10 | compose.local.yml | ✅ RÉSOLU | [`group_add` dupliqués par le merge compose.yml + compose.local.yml](#erreur-14--group_add-dupliques-par-le-merge-composeyml--composelocalyml) |
| 13 | 2026-05-21 | OpenCV 4.10 / camera | ✅ RÉSOLU | [`CV_AUTOSTEP` pas exposé transitivement sur OpenCV 4.10 Linux](#erreur-13--cv_autostep-pas-expose-transitivement-sur-opencv-410-linux) |
| 12 | 2026-05-21 | apt / Catch2 | ✅ RÉSOLU | [Catch2 v3 requis mais apt Jammy fournit v2.13 — compile from source](#erreur-12--catch2-v3-requis-mais-apt-jammy-fournit-v213--compile-from-source) |
| 11 | 2026-05-21 | CMakeLists.txt / Linux | ✅ RÉSOLU | [`CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` casse `FindOpenGL` sur Linux](#erreur-11--cmake_try_compile_target_type-static_library-casse-findopengl-sur-linux) |
| 10 | 2026-05-21 | apt / OpenGL pour Qt6 | ✅ RÉSOLU | [Qt6Gui ne trouve pas OpenGL — paquets `lib*-dev` manquants](#erreur-10--qt6gui-ne-trouve-pas-opengl--paquets-lib-dev-manquants) |
| 9 | 2026-05-21 | ONNX Runtime build from source | ✅ RÉSOLU | [Build ONNX Runtime ARM64 — 4 sous-pièges (CMake, clone, Eigen hash)](#erreur-9--build-onnx-runtime-arm64--4-sous-pieges-cmake-clone-eigen-hash) |
| 8 | 2026-05-21 | build_jetson.sh / Qt6 multiarch | ✅ RÉSOLU | [Qt6 introuvable côté CMake — `CMAKE_PREFIX_PATH` sans multiarch](#erreur-8--qt6-introuvable-cote-cmake--cmake_prefix_path-sans-multiarch) |
| 7 | 2026-05-21 | CMakeLists.txt / Jammy | ✅ RÉSOLU | [`cmake_minimum_required(VERSION 3.28)` cassait Jammy CMake 3.22.1](#erreur-7--cmake_minimum_requiredversion-328-cassait-jammy-cmake-3221) |
| 6 | 2026-05-21 | compose.yml / devices | ✅ RÉSOLU | [`/dev/video0` map empêche le container de démarrer sans caméra branchée](#erreur-6--devvideo0-map-empeche-le-container-de-demarrer-sans-camera-branchee) |
| 5 | 2026-05-21 | dev.Dockerfile / vcpkg | ✅ RÉSOLU | [`bootstrap-vcpkg.sh` échoue sur ARM64 — vcpkg désactivé par défaut](#erreur-5--bootstrap-vcpkgsh-echoue-sur-arm64--vcpkg-desactive-par-defaut) |
| 4 | 2026-05-21 | apt / Qt6 base.Dockerfile | ✅ RÉSOLU | [`qt6-virtualkeyboard` n'est qu'un nom de paquet source sur Jammy](#erreur-4--qt6-virtualkeyboard-nest-quun-nom-de-paquet-source-sur-jammy) |
| 3 | 2026-05-21 | Docker / kernel Tegra | ✅ RÉSOLU | [Docker 29.x sur JP6.2 — `iptable_raw` manquant dans le kernel Tegra](#erreur-3--docker-29x-sur-jp62--iptable_raw-manquant-dans-le-kernel-tegra) |
| 2 | 2026-05-21 | Docker / image base | ✅ RÉSOLU | [Repo `dustynv/l4t-jetpack` n'existe pas sur Docker Hub](#erreur-2--repo-dustynvl4t-jetpack-nexiste-pas-sur-docker-hub) |
| 1 | 2026-05-08 | ONNX Runtime / apt ARM64 | ✅ RÉSOLU | [libonnxruntime-dev absent en apt Ubuntu 22.04 ARM64 — résolu via compile from source dans le base.Dockerfile (cf #9)](#erreur-1--libonnxruntime-dev-absent-en-apt-ubuntu-2204-arm64) |

**Statuts possibles** :
- 🔴 OUVERT — pas encore résolu
- 🟡 CONTOURNÉ — solution temporaire en place
- ✅ RÉSOLU — fix appliqué et validé
- 📝 INFO — note pour mémoire (pas un bug, juste un piège)

---

## Modèle pour nouvelle entrée

Format à suivre pour chaque nouvelle erreur :

```markdown
## ERREUR N — Titre court et explicite

**Date :** YYYY-MM-DD
**Composant :** [Docker / CMake / Qt / OpenCV / TensorRT / V4L2 / RealSense / GUI / etc.]
**Statut :** 🔴 OUVERT | 🟡 CONTOURNÉ | ✅ RÉSOLU | 📝 INFO
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session YYYY-MM-DD

### Symptôme
Description de ce qui se passe (logs, messages d'erreur exacts, comportement observé).

```
[coller les logs/messages d'erreur ici, tel quel]
```

### Contexte
- Commande lancée : `...`
- État de l'environnement : `...`
- Reproductible : oui / non / parfois

### Cause
Explication technique de la racine du problème (une fois identifiée).

### Ce qui n'a PAS fonctionné
- Tentative 1 : ...
- Tentative 2 : ...

### Solution appliquée ✅
Fix concret avec les commandes/diff exacts.

```bash
# commandes
```

```diff
- ancien code
+ nouveau code
```

### Notes / prévention
Comment éviter ce problème à l'avenir, ou quels symptômes apparentés guetter.
```

---

## Pièges anticipés (à confirmer/refuter par l'expérience)

Ces points sont **anticipés** mais pas encore observés. À convertir en vraie entrée d'erreur si rencontrés.

### Probabilité élevée
- **Build OpenCV CUDA OOM** sur Jetson 32GB en `-j$(nproc)` (12 cores) — la solution est de limiter à `-j6` voire `-j4`. Le `scripts/build_jetson.sh` a déjà un cap à 6, mais le `base.Dockerfile` lui utilise `-j$(nproc)` au build de OpenCV — à adapter si OOM.
- **Tag `dustynv/l4t-jetpack:r36.4.0` n'existe pas** : le tag exact dépend de la version mineure JP6.2. Vérifier sur [hub.docker.com/r/dustynv/l4t-jetpack/tags](https://hub.docker.com/r/dustynv/l4t-jetpack/tags) et ajuster `L4T_VERSION` dans `compose.yml` build args.
- **`qt6-virtualkeyboard` non disponible** sur Ubuntu 22.04 — le paquet peut s'appeler `qml6-module-qtquick-virtualkeyboard` ou autre. À tester.

### Probabilité moyenne
- **`libonnxruntime-dev` non packagé en apt Ubuntu 22.04 ARM64** : actuellement avec `|| true` dans `base.Dockerfile` pour ne pas casser le build. Il faudra le compiler from source ou utiliser le wheel pip ARM si on veut réellement ONNX Runtime.
- **`#ifdef _WIN32` non encadré dans certains fichiers** : un grep complet détectera ceux qui ne sont pas dans la branche Linux. Probablement [src/ai/InferenceEngine.cpp:68](../src/ai/InferenceEngine.cpp#L68) (chargement DLL TRT) à adapter.
- **vcpkg `arm64-linux` triplet manquant des dépendances** : certains paquets vcpkg ne sont pas testés sur ARM. Si nécessaire, utiliser apt à la place.
- **Permissions `/dev/video0`** dans le container : si non accessible, vérifier `group_add: video` ET que le user host est dans `video`.

### Probabilité faible mais à surveiller
- **X11 forwarding dans le container** : peut nécessiter `xhost +SI:localuser:root` plutôt que `xhost +local:docker` selon la version Docker.
- **Driver Wayland** au lieu de X11 — Ubuntu 22.04 sur L4T = X11 par défaut, mais à vérifier (`echo $XDG_SESSION_TYPE`).
- **Throttling thermique** sous charge prolongée si ventilateur insuffisant en mode MAXN.
- **Engines TRT générés auto au premier run** : le binaire doit gérer ça (option `--build-engines` ou auto au premier appel inference). Actuellement non implémenté côté C++, à voir.

---

<!-- AJOUTER LES NOUVELLES ERREURS AU-DESSUS DE CETTE LIGNE -->

## ERREUR 17 — `Found 0 camera(s)` sur device V4L2 fonctionnel — énumération via QMediaDevices

**Date :** 2026-06-14
**Composant :** Application.cpp / énumération caméra (Qt Multimedia vs OpenCV V4L2)
**Statut :** 🟡 CONTOURNÉ (fix appliqué, validation Jetson en attente)
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-06-14

### Symptôme
Caméra microscope HAYEAR (MOS-4K Pro, `0ac8:3420`) branchée, `/dev/video0` présent et
mappé dans le container, OpenCV l'ouvre parfaitement — mais l'app affiche « No camera
detected » et logue `Found 0 camera(s)`. Aucun flux ni device dans l'UI.

```
[2026-06-14 19:36:20.207] [info] [:] Found 0 camera(s)
```

Preuve que le matériel est OK (dans le container) :
```
$ python3 -c "import cv2; c=cv2.VideoCapture(0, cv2.CAP_V4L2); print(c.isOpened(), c.read()[0])"
True True
$ v4l2-ctl -d /dev/video0 --list-formats-ext   # MJPG 1920x1080@30 + H264 + 4K
```

### Cause
`Application::initialize()` énumérait les caméras avec **`QMediaDevices::videoInputs()`**
(backend Qt6 Multimedia, GStreamer/FFmpeg). Sur Jetson dans Docker, ce backend ne voit pas
les `/dev/video*` (plugins GStreamer caméra absents/non configurés), donc liste vide. Or la
capture réelle (`CameraCapture::captureLoop`) ouvre le device **par index via OpenCV/V4L2**,
qui fonctionne. L'énumération et la capture n'utilisaient pas le même backend. Pire :
`CameraCapture::listDevices()` (énumération OpenCV V4L2, correcte) existait déjà mais
n'était pas appelée.

### Solution
Énumérer via `camera::CameraCapture::listDevices()` (même backend que la capture) au lieu de
`QMediaDevices`. Ce dernier n'est plus interrogé que pour fournir un libellé lisible quand il
expose le device. Patch dans `Application.cpp` (bloc « Enumerate cameras »).

> Reste à valider : rebuild + lancement sur Jetson, vérifier que la caméra apparaît dans le
> sélecteur et que le flux s'affiche. Détail format : la caméra streame MJPG/H264/H265 ;
> `captureLoop` force déjà `FOURCC MJPG` (CLAUDE.md piège FOURCC). À passer ✅ RÉSOLU une fois
> le flux confirmé à l'écran.

---

## ERREUR 16 — Link `undefined reference HPDF_*` — header présent mais lib non linkée

**Date :** 2026-06-14
**Composant :** CMake / libharu (génération PDF rapports)
**Statut :** ✅ RÉSOLU (fix CMake validé sur Jetson — build + 7/7 ctest, 2026-06-14)
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-06-14

### Symptôme
Build de la PR #5 (`l0l0l0/Assistant:claude/focused-fermi-if21mm`) dans le container dev :
link de `MicroscopeIBOM` échoue avec des dizaines de `undefined reference to 'HPDF_*'`
(`HPDF_New`, `HPDF_AddPage`, `HPDF_Page_TextOut`…) provenant de
`ReportGenerator::generatePDF`.

### Contexte
- Commande : `bash scripts/build_jetson.sh` (étape `[18/18] Linking`)
- `src/export/ReportGenerator.cpp` active le code PDF via `#if __has_include(<hpdf.h>)` →
  le header libharu **est** présent dans l'image (`apt libhpdf-dev`), donc les appels
  `HPDF_*` sont compilés.
- Côté CMake, le bloc libharu ne teste que `find_package(unofficial-libharu)`,
  `find_package(libharu)` et `find_package(HPDF)`. Sur Jetson Jammy, le paquet apt
  `libhpdf-dev` ne fournit **ni** config CMake **ni** module `FindHPDF`, seulement
  `libhpdf.so` + `<hpdf.h>`. Aucun `find_package` ne réussit → la lib n'est jamais
  ajoutée au `target_link_libraries` → symboles non résolus au link.

### Cause
Mismatch entre la détection côté `.cpp` (`__has_include`, basée sur le header) et la
détection côté CMake (`find_package`, basée sur un package config absent). Le code se
compile mais la lib ne se linke pas.

### Solution
Ajouter un fallback `find_library(HPDF_LIBRARY NAMES hpdf)` dans `CMakeLists.txt` quand
aucun `find_package` n'aboutit, et une branche `elseif(HPDF_LIBRARY)` qui linke
`${HPDF_LIBRARY}` + définit `IBOM_HAS_LIBHARU`. Cela aligne CMake sur le même critère
que `__has_include` dans le `.cpp`.

```cmake
find_package(HPDF QUIET)
if(NOT HPDF_FOUND)
    find_library(HPDF_LIBRARY NAMES hpdf)
endif()
...
elseif(HPDF_LIBRARY)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${HPDF_LIBRARY})
    target_compile_definitions(${PROJECT_NAME} PRIVATE IBOM_HAS_LIBHARU)
endif()
```

Reconfigure propre nécessaire (`rm -rf build/CMakeCache.txt build/CMakeFiles`) pour
réévaluer le `find_library`. **Validé sur Jetson le 2026-06-14** : build OK
(binaire 1,4 MB) + `ctest` 7/7.

> Note : le fix vit sur la branche `claude/pensive-euler-pvde0v`. La PR #5 elle-même
> reste à corriger (idéalement intégrer ce patch CMake côté PR avant merge).

---

## ERREUR 15 — Caméra USB vue par lsusb mais "No camera detected" dans l'app — /dev/video* non mappés

**Date :** 2026-06-10
**Composant :** compose.local.yml / caméra USB
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-06-10 (suite 4)

### Symptôme
La caméra microscope apparaît côté hôte (`lsusb` : `Bus 001 Device 008: ID 0ac8:3420 Z-Star Microelectronics Corp. Venus USB2.0 Camera`) mais l'application dans le container affiche "No camera detected" (Settings → Camera).

### Contexte
- App lancée via `scripts/run_local_gui.sh` (container dev)
- Reproductible : oui

### Cause
Pas un bug : les mappings `/dev/video*` étaient **volontairement commentés** dans `docker/compose.local.yml` ("décommenter quand la caméra USB est branchée") — décision prise quand la caméra n'était pas encore là (cf erreur #6 : un device mappé mais absent empêche le container de démarrer). Le container n'avait donc aucun accès au périphérique vidéo.

### Solution appliquée ✅
**v1 (même jour, remplacée)** : `/dev/video0`+`/dev/video1` mappés en dur dans `compose.local.yml` → refusé par l'utilisateur car caméra débranchée = container ne démarre plus (revers de l'erreur #6).

**v2 (définitive)** : mapping **dynamique** — `run_local_gui.sh` génère `/tmp/microscope-ibom.cameras.yml` avec les `/dev/video*` réellement présents au lancement et l'ajoute aux `-f` compose :
- caméra absente → pas d'override → l'app démarre **sans** caméra (liste vide, aucun échec)
- caméra branchée après coup → relancer le script (~10 s, le container est recréé avec les devices)
- gère N nœuds vidéo (UVC video0+video1, future RealSense video2+)

Plus aucun `/dev/video*` en dur dans `compose.local.yml`. Hot-plug complet (sans relancer le script) = option future via `device_cgroup_rules c 81:* rmw` + montage `/dev` — non retenu pour l'instant (effets de bord à tester).

### Notes
- Caméra **USB 2.0** (Z-Star 0ac8:3420) ⇒ MJPG indispensable pour 1080p@30 (la bande passante USB 2.0 ne permet pas le YUYV 1080p) — la demande `CAP_PROP_FOURCC=MJPG` est déjà dans `CameraCapture.cpp` depuis le commit `e174286`. Vérifier le FOURCC réel dans le log au premier lancement.
- Si la **liste** de caméras de l'app reste vide alors que la capture par index marche : l'énumération passe par `QMediaDevices` (Qt/GStreamer), plus fragile en container que le probing V4L2 d'OpenCV — patch d'énumération à prévoir le cas échéant.

---

## ERREUR 14 — `group_add` dupliqués par le merge compose.yml + compose.local.yml

**Date :** 2026-06-10
**Composant :** Docker Compose / compose.local.yml
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-06-10

### Symptôme
Au lancement via `scripts/run_local_gui.sh` (qui empile `-f docker/compose.yml -f docker/compose.local.yml`), les groupes `video` et `plugdev` apparaissent en double dans la config du service — l'utilisateur a dû éditer `compose.local.yml` à la main sur le Jetson pour démarrer.

### Contexte
- Commande lancée : `docker compose -f docker/compose.yml -f docker/compose.local.yml up -d --force-recreate dev`
- Reproductible : oui (déterministe, vient de la sémantique de merge compose)

### Cause
Docker Compose **concatène** les listes (`group_add`, `devices`, `volumes`, …) entre le fichier de base et l'override — il ne les remplace pas et ne déduplique pas. `compose.yml` déclare déjà `group_add: [video, plugdev, dialout]` pour `dev` (et `[video, plugdev]` pour `runtime`) ; `compose.local.yml` re-déclarait `[video, input, plugdev]` → doublons `video` + `plugdev` dans la config fusionnée.

### Solution appliquée ✅
`compose.local.yml` ne déclare plus que ce qui est réellement **ajouté** par le workflow local, c'est-à-dire `input` :

```diff
     group_add:
-      - video
       - input
-      - plugdev
```

(appliqué aux services `dev` et `runtime` + commentaire en tête de fichier expliquant la sémantique de merge).

⚠️ Sur le Jetson, le fichier avait été modifié localement : faire `git checkout docker/compose.local.yml` avant le `git pull` qui ramène ce fix.

---

## ERREUR 13 — `CV_AUTOSTEP` pas exposé transitivement sur OpenCV 4.10 Linux

**Date :** 2026-05-21
**Composant :** UnifiedAllocator.cpp / OpenCV 4.10
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
```
src/camera/UnifiedAllocator.cpp:95:41: error: 'CV_AUTOSTEP' was not declared in this scope
```

### Cause
`CV_AUTOSTEP` est une sentinelle de l'API C legacy d'OpenCV (valeur `0x7fffffff`) utilisée par `cv::MatAllocator::allocate` pour signaler "step auto-calcule". Définie dans `opencv2/core/types_c.h`. Sur Windows OpenCV 4.12 (vcpkg) ce header est exposé transitivement via `<opencv2/core.hpp>`, mais sur Linux OpenCV 4.10 (apt jammy / compile from source dans base.Dockerfile) ce n'est pas le cas.

### Solution appliquée ✅
[src/camera/UnifiedAllocator.cpp](../src/camera/UnifiedAllocator.cpp) : include explicite + fallback define :
```cpp
#include <opencv2/core/types_c.h>
#include <opencv2/core/mat.hpp>

#ifndef CV_AUTOSTEP
#define CV_AUTOSTEP ((size_t)0x7fffffff)
#endif
```

Le fallback garantit la compilation même si un futur OpenCV deprecate le header.


## ERREUR 12 — Catch2 v3 requis mais apt Jammy fournit v2.13 — compile from source

**Date :** 2026-05-21
**Composant :** apt / Catch2 / base.Dockerfile
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
```
CMake Error: Could not find a configuration file for package "Catch2" compatible with version "3".
/usr/lib/cmake/Catch2/Catch2Config.cmake, version: 2.13.8
```

### Cause
Les tests du projet (`tests/test_unified_allocator.cpp` etc.) incluent `catch2/catch_test_macros.hpp` — API v3, pas compatible avec v2.13 fournie par le paquet `catch2` Jammy.

### Solution appliquée ✅
[docker/base.Dockerfile](../docker/base.Dockerfile) :
- Retire `catch2` de l'apt install (qui installe v2)
- Ajoute un RUN qui compile Catch2 v3.5.4 from source (pattern identique à ZXing-cpp, ~3 min)

```dockerfile
RUN cd /tmp \
    && git clone --depth 1 --branch v3.5.4 https://github.com/catchorg/Catch2.git \
    && cd Catch2 && cmake -B build -G Ninja \
       -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_TESTING=OFF \
       -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build build -j$(nproc) \
    && cmake --install build
```


## ERREUR 11 — `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` casse `FindOpenGL` sur Linux

**Date :** 2026-05-21
**Composant :** CMakeLists.txt / Linux + Ninja
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
Au configure CMake du projet (alors qu'`OpenGL`, `Qt6` etc. sont bien installés) :
```
-- Could NOT find OpenGL (missing: OPENGL_opengl_LIBRARY OPENGL_glx_LIBRARY)
-- Could NOT find WrapOpenGL (missing: WrapOpenGL_FOUND)
Qt6Gui could not be found because dependency WrapOpenGL could not be found.
```
**Mais** un test isolé `find_package(Qt6)` dans un mini-CMakeLists trouve `OpenGL: /usr/lib/aarch64-linux-gnu/libOpenGL.so` parfaitement.

### Cause
[CMakeLists.txt:6](../CMakeLists.txt#L6) posait `set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)` inconditionnellement. C'était un workaround pour un bug CMake 4.x + Ninja sur Windows (`rules.ninja` pas généré pendant try_compile). **Sur Linux**, ce flag empêche les `try_compile` de **linker** — `FindOpenGL` ne peut plus valider la présence des libs (qui requiert un test de lien effectif), donc retourne "NOT FOUND".

### Solution appliquée ✅
[CMakeLists.txt:6-13](../CMakeLists.txt#L6) : conditionner à Windows uniquement.
```cmake
if(WIN32)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()
```

### Notes / prévention
Si on revient un jour à Windows avec CMake 4.x + Ninja, ce flag reste actif. Sur tout autre setup, il est neutralisé.


## ERREUR 10 — Qt6Gui ne trouve pas OpenGL — paquets `lib*-dev` manquants

**Date :** 2026-05-21
**Composant :** apt / OpenGL / Qt6
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
Même symptôme que #11 (`Could NOT find OpenGL`) mais cause différente — apparaît même avec `CMAKE_TRY_COMPILE_TARGET_TYPE` correctement conditionné. `libqt6opengl6-dev` est installé mais Qt6 cherche aussi les libs OpenGL système sous-jacentes (`.so` + headers).

### Solution appliquée ✅
[docker/base.Dockerfile](../docker/base.Dockerfile) — ajout au RUN apt install du stage final :
```
libgl-dev libglx-dev libegl-dev libgles-dev libopengl-dev libglu1-mesa-dev libvulkan-dev
```

Cascade évitée : sans ces paquets, **Qt6Gui** échoue, et par dépendance **Qt6Widgets, Qt6OpenGL, Qt6OpenGLWidgets, Qt6Multimedia, Qt6MultimediaWidgets, Qt6PrintSupport** échouent aussi → `find_package(Qt6)` global KO.


## ERREUR 9 — Build ONNX Runtime ARM64 — 4 sous-pièges (CMake, clone, Eigen hash)

**Date :** 2026-05-21
**Composant :** docker / base.Dockerfile / onnxruntime-builder stage
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Contexte
Compilation d'ONNX Runtime v1.19.2 ARM64 from source dans un stage Docker (`onnxruntime-builder`) avec CUDA 12.6 + TensorRT 10.3 EP. Étape critique d'Option C choisie pour résoudre ERREUR #1 (pas de paquet `libonnxruntime-dev` sur Jammy ARM64). **~37 min de compile NVCC** au total.

### Sous-piège 9a — CMake version exigée par ORT vs Jammy
**Symptôme** : `CMake Error: CMake 3.26 or higher is required. You are running version 3.22.1` (Jammy fournit 3.22).

**Fix** : `pip3 install "cmake>=3.28,<4"` dans le stage onnxruntime-builder. Le binaire pip-installé `/usr/local/bin/cmake` passe devant `/usr/bin/cmake` dans PATH.

**ATTENTION CMake 4.x** : on borne `<4` parce que CMake 4.0 a supprimé le support de `cmake_minimum_required(VERSION <3.5)`, et plusieurs deps ORT fetched via FetchContent (google_nsync notamment) ont des vieux headers `<3.5`. CMake 4.x → "Compatibility with CMake < 3.5 has been removed" → build échoue. La fenêtre safe est `[3.26, 4)` = série 3.31.x.

### Sous-piège 9b — `git clone --recursive` timeout sur GitHub
**Symptôme** : `git clone --depth 1 --branch v1.19.2 --recursive ...` plante après 17 min :
```
error: RPC failed; curl 92 HTTP/2 stream 0 was not closed cleanly: CANCEL
fetch-pack: unexpected disconnect while reading sideband packet
```
ONNX Runtime + ~30 submodules ≈ 3-4 GB d'un coup, GitHub throttle/timeout.

**Fix** : décomposition + retry :
```dockerfile
RUN git config --global http.postBuffer 524288000 \
 && git config --global http.lowSpeedLimit 0 \
 && git config --global http.lowSpeedTime 999999 \
 && git clone --depth 1 --branch ${ONNXRUNTIME_VERSION} https://github.com/microsoft/onnxruntime.git \
 && cd onnxruntime \
 && for i in 1 2 3; do \
        git submodule update --init --recursive --depth 1 --jobs 4 && break ; \
        sleep 10 ; \
    done
```

### Sous-piège 9c — Eigen download hash mismatch (upstream bug)
**Symptôme** : 6 retries puis :
```
SHA1 hash of eigen-....zip does not match expected value
expected: be8be39fdbc6e60e94fa7870b280707069b5b81a
  actual: 32b145f525a8308d7ab1c09388b2e288312d8eba
CMake Error: Each download failed!
```
Bug **upstream documenté** : [microsoft/onnxruntime#26707](https://github.com/microsoft/onnxruntime/issues/26707). GitLab régénère les zip archives dynamiquement, le SHA1 hardcodé devient invalide. Touche **v1.17.1, v1.19.2, v1.20.1, v1.21.0, v1.22.0** — bumper la version ne résout rien.

**Fix** : patcher `cmake/deps.txt` (PAS `cmake/external/eigen.cmake` qui ne contient pas le hash) :
```dockerfile
RUN sed -i 's|be8be39fdbc6e60e94fa7870b280707069b5b81a|32b145f525a8308d7ab1c09388b2e288312d8eba|g' \
        cmake/deps.txt
```

À re-patcher si GitLab change à nouveau le contenu (rare).

### Sous-piège 9d — `build.sh` exigences pip
**Symptôme** initial : `Failed to import psutil. Please pip install psutil`. Pas bloquant mais améliore la parallélisation NVCC.

**Fix** : `pip3 install psutil` ajouté au même RUN que cmake.

### Résultat final
~37 min de compile, image base finale contient :
```
/usr/local/lib/libonnxruntime.so.1
/usr/local/lib/libonnxruntime_providers_cuda.so
/usr/local/lib/libonnxruntime_providers_tensorrt.so
/usr/local/lib/libonnxruntime_providers_shared.so
/usr/local/include/onnxruntime/*.h
/usr/local/lib/cmake/onnxruntime/onnxruntimeConfig.cmake
```
`find_package(onnxruntime CONFIG REQUIRED)` passe ✅.


## ERREUR 8 — Qt6 introuvable côté CMake — `CMAKE_PREFIX_PATH` sans multiarch

**Date :** 2026-05-21
**Composant :** scripts/build_jetson.sh
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
```
By not providing "FindQt6.cmake" in CMAKE_MODULE_PATH this project has
asked CMake to find a package configuration file provided by "Qt6", but
CMake did not find one.
```
Pourtant `qmake6 -query QT_VERSION` retourne `6.2.4` dans le container et `qt6-base-dev` est installé.

### Cause
Sur Ubuntu Jammy arm64, les fichiers CMake de Qt6 sont dans `/usr/lib/aarch64-linux-gnu/cmake/Qt6/Qt6Config.cmake` (path multiarch Debian/Ubuntu). Le `ENV CMAKE_PREFIX_PATH=/usr/local` posé par base.Dockerfile ne couvre pas ce path.

### Solution appliquée ✅
[scripts/build_jetson.sh](../scripts/build_jetson.sh) — export du path multiarch détecté via `dpkg-architecture` :
```bash
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo aarch64-linux-gnu)
export CMAKE_PREFIX_PATH="/usr/lib/${MULTIARCH}/cmake:${CMAKE_PREFIX_PATH:-}"
```

Généralisé pour x86_64 aussi via le triplet détecté.


## ERREUR 7 — `cmake_minimum_required(VERSION 3.28)` cassait Jammy CMake 3.22.1

**Date :** 2026-05-21
**Composant :** CMakeLists.txt / Jammy
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
```
CMake Error at CMakeLists.txt:1 (cmake_minimum_required):
  CMake 3.28 or higher is required.  You are running version 3.22.1
```

### Cause
Jammy fournit `cmake 3.22.1` via apt. Le projet exigeait `3.28` hérité du setup Windows (CMake 4.2.3 installé là-bas).

### Solution appliquée ✅
[CMakeLists.txt](../CMakeLists.txt) + [tests/CMakeLists.txt](../tests/CMakeLists.txt) : `cmake_minimum_required(VERSION 3.28)` → `(VERSION 3.22)`.

Le projet n'utilise aucune feature 3.23+ (pas de modules C++23, Qt 6.2 min CMake 3.16, `CMAKE_TRY_COMPILE_TARGET_TYPE` depuis 3.6). Rétrocompatible avec Windows CMake 4.2.3 (>= 3.22).

## ERREUR 6 — `/dev/video0` map empêche le container de démarrer sans caméra branchée

**Date :** 2026-05-21
**Composant :** docker/compose.yml / devices
**Statut :** ✅ RÉSOLU (devices commentés par défaut)
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
`docker compose up -d dev` plante immédiatement :
```
Error response from daemon: error gathering device information while adding
custom device "/dev/video0": no such file or directory
```

### Cause
`compose.yml` mapait `/dev/video0:/dev/video0` en hard requirement. Sans caméra USB branchée, ce device n'existe pas sur l'host → Docker refuse de créer le container. Or pour le dev/build C++ (sans test runtime caméra), on n'a pas besoin de ce device.

### Solution appliquée ✅
[docker/compose.yml](../docker/compose.yml) : bloc `devices:` commenté par défaut sur les services `dev` ET `runtime`, avec un commentaire explicatif. Décommenter quand le matériel est présent et qu'on en a besoin (runtime app, calibration, test caméra).

```diff
-    devices:
-      - /dev/video0:/dev/video0
-      - /dev/bus/usb:/dev/bus/usb
-      - /dev/input:/dev/input
-      - /dev/dri:/dev/dri
+    # devices: commentes par defaut, voir compose.yml pour les decommenter
```

### Notes / prévention
- Côté `dev` : le build C++ n'a besoin que de CUDA (déjà fourni via `runtime: nvidia`), pas de la caméra/écran. Container démarre tout seul.
- Côté `runtime` : quand on lancera l'app pour de vrai, il faudra décommenter `/dev/video0` (caméra) et `/dev/dri` (GPU display) au minimum.
- Solution future plus élégante : un script wrapper qui détecte les devices présents et génère un override compose (`docker-compose.override.yml`). Pas nécessaire pour le scope actuel.
- Le warning "Le runtime nvidia ne semble pas configure" affiché par `run-dev.sh` quand l'user n'est pas dans le groupe docker est un faux positif lié au même `permission denied` (à corriger plus tard, cosmétique).

## ERREUR 5 — `bootstrap-vcpkg.sh` échoue sur ARM64 — vcpkg désactivé par défaut

**Date :** 2026-05-21
**Composant :** dev.Dockerfile / vcpkg / ARM64
**Statut :** ✅ RÉSOLU (vcpkg rendu opt-in)
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
À l'étape 7/8 du bootstrap (build dev image), `bootstrap-vcpkg.sh` échoue avec un message générique qui liste les outils à installer pour toutes les distributions (Alpine, FreeBSD, OpenBSD, Solaris, etc.) :

```
sudo pacman -Syu base-devel git curl zip unzip tar cmake ninja
On Alpine: apk add build-base cmake ninja zip unzip curl git
On Solaris and illumos distributions: pkg install web/curl compress/zip compress/unzip
...
ERROR: process "...bootstrap-vcpkg.sh -disableMetrics..." exit code: 1
```

### Cause
`vcpkg-tool` n'a pas de binaire ARM64 pré-compilé pour tous les systèmes. Le script `bootstrap-vcpkg.sh` fallback alors sur une compilation from source qui requiert un environnement précis. Sur l'image Ubuntu Jammy ARM64 dérivée de `nvcr.io/nvidia/l4t-jetpack`, soit une dépendance manque, soit le binaire n'est juste pas distribué pour cette cible exotique.

### Cause profonde / pourquoi c'est pas grave
**Sur Jetson, on n'utilise PAS vcpkg de toute façon.** Toutes les dépendances du projet sont déjà fournies par les paquets apt système installés dans `base.Dockerfile` :
- Qt6 → `qt6-base-dev` + co
- OpenCV CUDA → compilé from source dans `base.Dockerfile`
- spdlog, nlohmann_json, libharu, Catch2, ZXing → tous via apt
- ONNX Runtime → géré séparément (cf. ERREUR 1 anticipée)

Le `scripts/build_jetson.sh` détecte automatiquement la présence/absence de `VCPKG_ROOT` et adapte le toolchain CMake en conséquence.

### Solution appliquée ✅
[docker/dev.Dockerfile](../docker/dev.Dockerfile) : `ARG INSTALL_VCPKG=true` → `false` par défaut. Le bloc `RUN` est conservé pour permettre l'activation à la demande :

```bash
# Si on veut vcpkg malgre tout :
docker compose -f docker/compose.yml build dev --build-arg INSTALL_VCPKG=true
```

Le `ENV VCPKG_ROOT=/opt/vcpkg` et `ENV PATH=...` ont aussi été retirés inconditionnellement, pour que `build_jetson.sh` fallback proprement sur les paquets apt système quand vcpkg n'est pas là.

### Notes / prévention
- vcpkg n'a jamais été pertinent côté Jetson. Le code initial dérivait du setup Windows où vcpkg apportait `onnxruntime-gpu`, `opencv4`, `qt6-base`, etc. Sur Jetson on a tous ces paquets en apt natif (ARM64 build) — vcpkg n'apporterait que de la redondance.
- Si un jour on a besoin d'une dépendance spécifique non packagée en apt, mieux vaut compiler from source dans le `base.Dockerfile` (comme on fait déjà pour OpenCV CUDA, librealsense, ZXing-cpp) qu'introduire vcpkg.

## ERREUR 4 — `qt6-virtualkeyboard` n'est qu'un nom de paquet source sur Jammy

**Date :** 2026-05-21
**Composant :** apt / base.Dockerfile / Qt6
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
À l'étape 6/8 du bootstrap (build de l'image `microscope-ibom:base`), l'install Qt6 dans le Dockerfile plante :

```
8.021 E: Unable to locate package qt6-virtualkeyboard
failed to solve: process "/bin/sh -c apt-get update && apt-get install -y --no-install-recommends ..." exit code: 100
[ERROR] Build base echec
```

### Cause
Sur Ubuntu 22.04 (Jammy) arm64, `qt6-virtualkeyboard` est un nom de paquet **source** uniquement — il n'y a pas de paquet binaire de ce nom. Les binaires sont éclatés en plusieurs paquets (cf. [packages.ubuntu.com/jammy/qml6-module-qtquick-virtualkeyboard](https://packages.ubuntu.com/jammy/qml6-module-qtquick-virtualkeyboard)) :
- `qt6-virtualkeyboard-plugin` — le plugin Qt chargé automatiquement
- `qml6-module-qtquick-virtualkeyboard` — les composants QML
- `libqt6virtualkeyboard6-dev` — headers (uniquement si on compile contre l'API)

### Solution appliquée ✅
[docker/base.Dockerfile](../docker/base.Dockerfile) — bloc Qt6 patché :
```diff
-    qt6-virtualkeyboard \
+    qt6-virtualkeyboard-plugin \
+    qml6-module-qtquick-virtualkeyboard \
```

Pas besoin de `libqt6virtualkeyboard6-dev` : on ne compile pas contre l'API publique du virtual keyboard, on l'utilise juste comme plugin pour l'écran tactile Minix SF16T.

### Validation préalable
Avant de relancer le build (90 min), tous les autres paquets Qt6 de la liste du Dockerfile ont été vérifiés via `apt-cache show` sur le host Jetson Jammy → ✅ tous présents (`qt6-shader-baker`, `qml6-module-qtquick`, `qml6-module-qtquick-controls`, etc.). Pas de risque d'autre échec sur cette étape `apt install`.

### Notes / prévention
- Toujours vérifier l'existence des paquets via `apt-cache show` AVANT de lancer un build long (les paquets `*-dev` source vs binaires différents = piège fréquent côté Ubuntu).
- Le piège était anticipé en haut de ce document ("`qt6-virtualkeyboard` non disponible sur Ubuntu 22.04"). Confirmé en pratique.
- Le cache Docker préserve le travail du stage `opencv-builder` qui avait déjà avancé — le rebuild ne repartira pas de zéro.

## ERREUR 3 — Docker 29.x sur JP6.2 — `iptable_raw` manquant dans le kernel Tegra

**Date :** 2026-05-21
**Composant :** Docker / kernel Tegra / réseau bridge
**Statut :** ✅ RÉSOLU (contournement par `--network host`)
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
N'importe quel `docker run` (avec ou sans `--runtime nvidia`) sur bridge network échoue :

```
docker: Error response from daemon: failed to set up container networking:
failed to create endpoint X on network bridge: Unable to enable DIRECT ACCESS FILTERING - DROP rule:
(iptables failed: iptables --wait -t raw -A PREROUTING -d 172.17.0.2 ! -i docker0 -j DROP:
iptables v1.8.7 (legacy): can't initialize iptables table `raw': Table does not exist (do you need to insmod?)
Perhaps iptables or your kernel needs to be upgraded.
 (exit status 3))
```

Survient à l'étape 3/8 du bootstrap (test runtime nvidia) mais affecterait aussi tous les `docker compose build` (qui font des `RUN apt-get update` sur bridge).

### Contexte
- Jetson AGX Orin 32GB Seeed, JetPack 6.2 (L4T R36.4.3)
- Kernel : `5.15.148-tegra`
- Docker installé via `apt install docker.io` → version `29.1.3-0ubuntu3~22.04.2` (jammy-updates)
- `nvidia-container-toolkit 1.16.2-1` correctement configuré
- L'image `nvcr.io/nvidia/l4t-jetpack:r36.4.0` se pull sans souci — le problème est purement réseau

### Cause
Docker 28.x+ utilise des règles `iptables -t raw -A PREROUTING ...` pour son bridge networking. La table `raw` nécessite le module kernel `iptable_raw`, **non compilé dans le kernel Tegra de JP6.2** :

```bash
$ sudo find /lib/modules/$(uname -r) -name "iptable_raw*"
# (aucun fichier)
$ sudo modprobe iptable_raw
modprobe: FATAL: Module iptable_raw not found in directory /lib/modules/5.15.148-tegra
$ lsmod | grep iptable
iptable_nat            16384  1
iptable_filter         16384  1     # ← seulement nat et filter, pas raw
```

C'est la classe de problème documentée par NVIDIA dans [Error with Nvidia Container Runtime / Docker Integration on AGX Orin JP6.2](https://forums.developer.nvidia.com/t/error-with-nvidia-container-runtime-with-docker-integration-on-agx-orin-with-jp6-2/324558/17) (réponse AastaLLL #17 : "Docker v28.0.0 requires more kernel config which is not enabled on r36.4.3 by default" → `CONFIG_IP_SET`, `iptable_raw`, etc.).

### Solutions évaluées
| Option | Verdict |
|--------|---------|
| Downgrade `docker.io` à 27.5.1 (recommandation officielle NVIDIA) | ❌ Pas dispo dans apt Jammy ; il faudrait passer à `docker-ce` du repo Docker Inc — gros remaniement |
| Downgrade `docker.io` à 20.10.12 (seule version Jammy pré-28) | ❌ Trop ancien, pas compat nvidia-container-toolkit 1.16 |
| `modprobe iptable_raw` | ❌ Module absent du kernel Tegra |
| Recompiler le kernel Jetson avec `CONFIG_IPTABLE_RAW=m` | ❌ Trop invasif pour ce projet |
| Désactiver iptables dans daemon.json (`"iptables": false`) | ❌ Casse tout le bridge networking, sécurité dégradée |
| **`--network host` partout** | ✅ **Retenu** : notre stack utilise déjà `network_mode: host` partout en runtime. Cohérent. Aucun changement système. |

### Solution appliquée ✅
1. **[scripts/bootstrap_jetson.sh](../scripts/bootstrap_jetson.sh)** — ajout de `--network host` au test runtime nvidia (étape 3/8) :
   ```diff
   - if docker_cmd run --runtime nvidia --rm "nvcr.io/nvidia/l4t-jetpack:${L4T_VERSION}" nvidia-smi
   + if docker_cmd run --runtime nvidia --network host --rm "nvcr.io/nvidia/l4t-jetpack:${L4T_VERSION}" nvidia-smi
   ```
2. **[docker/compose.yml](../docker/compose.yml)** — ajout de `build.network: host` aux trois services (`base`, `dev`, `runtime`) pour que `docker compose build` utilise le host networking pendant les `RUN apt-get update` internes :
   ```yaml
   base:
     build:
       context: ..
       dockerfile: docker/base.Dockerfile
       network: host          # ← ajoute
       args: ...
   ```

### Validation
Test manuel après ajout de `--network host` :
```bash
$ sudo docker run --rm --network host --runtime nvidia nvcr.io/nvidia/l4t-jetpack:r36.4.0 nvidia-smi
NVIDIA-SMI 540.4.0     Driver Version: 540.4.0     CUDA Version: 12.6
| 0  Orin (nvgpu)    N/A                            ...
```
GPU accessible, CUDA 12.6 OK.

### Notes / prévention
- **Tout futur `docker run` ou `docker build` sur ce Jetson doit forcer le host networking** tant que JP6.2 / kernel 5.15.148-tegra reste en place.
- Sur JP7.x (futur), si le kernel inclut `iptable_raw`, on pourra revenir au bridge. À re-tester au moment de la migration.
- `nvidia-smi` retourne `N/A` pour la plupart des métriques sur Tegra — c'est NORMAL (l'outil est conçu pour dGPU). Le seul fait qu'il retourne sans erreur + affiche "Orin (nvgpu)" + CUDA version valide le runtime.
- Le `network: host` au build est une feature de docker compose v2 (spec compose). Ne pas confondre avec `network_mode: host` qui est pour le runtime du service.

## ERREUR 2 — Repo `dustynv/l4t-jetpack` n'existe pas sur Docker Hub

**Date :** 2026-05-21
**Composant :** Docker / bootstrap / base.Dockerfile / compose.yml
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-21

### Symptôme
À l'étape 3/8 du bootstrap (test runtime nvidia) :

```
[bootstrap] Test runtime nvidia (peut declencher un pull d'image, ~quelques minutes)...
Unable to find image 'dustynv/l4t-jetpack:r36.4.0' locally
docker: Error response from daemon: pull access denied for dustynv/l4t-jetpack, repository does not exist or may require 'docker login'
[ERROR] Test runtime nvidia echec
```

Reproductible en manuel : `sudo docker run --runtime nvidia --rm dustynv/l4t-jetpack:r36.4.0 nvidia-smi` → même erreur.

### Contexte
- Jetson AGX Orin 32GB Seeed, JetPack 6.2 (L4T R36.4.3)
- `nvidia-container-toolkit 1.16.2-1` installé, `/etc/docker/daemon.json` configuré correctement (runtime nvidia présent)
- L'image `dustynv/l4t-jetpack` est référencée dans :
  - [scripts/bootstrap_jetson.sh:48](../scripts/bootstrap_jetson.sh#L48) : `L4T_VERSION="${L4T_VERSION:-r36.4.0}"`
  - [scripts/bootstrap_jetson.sh:201](../scripts/bootstrap_jetson.sh#L201) : `dustynv/l4t-jetpack:${L4T_VERSION}`
  - [docker/base.Dockerfile](../docker/base.Dockerfile) : `FROM dustynv/l4t-jetpack:${L4T_VERSION}`
  - [docker/compose.yml](../docker/compose.yml) : build args
- Le piège était **anticipé** dans la liste "Probabilité élevée" en haut de ce document.

### Cause
Le namespace `dustynv` héberge bien des images Jetson (l4t-pytorch, jetson-inference, etc.) mais **PAS `l4t-jetpack`**. Ce nom de repo a été supposé à tort en se basant sur la convention NVIDIA officielle, qui elle utilise `nvcr.io/nvidia/l4t-jetpack`.

### Solution appliquée ✅
Tag identifié via `docker manifest inspect` côté Jetson — résultats :

| Tag testé | Existe ? |
|-----------|----------|
| `nvcr.io/nvidia/l4t-jetpack:r36.4.0` | ✅ EXISTS |
| `nvcr.io/nvidia/l4t-jetpack:r36.4.3` | ❌ not found |
| `nvcr.io/nvidia/l4t-jetpack:r36.3.0` | ✅ EXISTS |
| `nvcr.io/nvidia/l4t-base:r36.2.0` | ✅ EXISTS |
| `dustynv/l4t-pytorch:r36.4.0` | ✅ EXISTS |

`r36.4.0` retenu car compatible ABI avec L4T R36.4.3 du Jetson (NVIDIA garantit la compat dans la série mineure).

Patches appliqués (commit à venir) :
- [docker/base.Dockerfile](../docker/base.Dockerfile) : 3× `FROM dustynv/...` → `FROM nvcr.io/nvidia/...`
- [scripts/bootstrap_jetson.sh](../scripts/bootstrap_jetson.sh) : commande de test runtime
- [docker/README.md](../docker/README.md) : exemple `docker run` dans la doc
- [docs/JETSON_MIGRATION.md](JETSON_MIGRATION.md) : squelettes du plan (cohérence)

`docker/compose.yml` n'avait pas besoin de patch — la `L4T_VERSION` y est juste un build arg passé au Dockerfile, et `r36.4.0` reste valide.

### Pour relancer après le fix (côté Jetson)
```bash
cd ~/Assistant-git && git pull
SKIP_PERFMODE=1 bash scripts/bootstrap_jetson.sh
```
`SKIP_PERFMODE=1` parce que MAXN est déjà actif et qu'on évite un re-set inutile. Docker + nvidia-container-toolkit déjà installés ne seront pas refaits.

### Notes / prévention
- L'image `nvcr.io/nvidia/l4t-jetpack` ne nécessite généralement PAS de NGC login pour pull (elle est publique).
- Pour vérifier un tag avant pull : `sudo docker manifest inspect nvcr.io/nvidia/l4t-jetpack:rX.Y.Z`
- Côté bootstrap : le script s'arrête proprement avec un message d'erreur clair, c'est bien — pas de cleanup nécessaire avant de relancer après patch.
- Pour relancer après fix : `SKIP_PERFMODE=1 bash ~/Assistant-git/scripts/bootstrap_jetson.sh` (Docker + nvidia-toolkit déjà installés, ne sera pas refait).

## ERREUR 1 — libonnxruntime-dev absent en apt Ubuntu 22.04 ARM64

**Date :** 2026-05-08 (anticipé) / 2026-05-21 (résolu)
**Composant :** ONNX Runtime / build base.Dockerfile
**Statut :** ✅ RÉSOLU — Option C (compile from source) retenue, voir [ERREUR 9](#erreur-9--build-onnx-runtime-arm64--4-sous-pieges-cmake-clone-eigen-hash) pour le détail des 4 sous-pièges et le fix final.
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) sessions 2026-05-08 (suite) + 2026-05-21

### Symptôme attendu
Au build de `microscope-ibom:base`, l'instruction suivante dans `docker/base.Dockerfile` :

```dockerfile
RUN apt-get install -y libonnxruntime-dev || true
```

va silencieusement échouer (le `|| true` masque l'erreur) car le paquet **n'existe pas** dans les dépôts Ubuntu 22.04 ARM64. Conséquence : au build C++, `find_package(onnxruntime CONFIG REQUIRED)` dans `CMakeLists.txt:75` va échouer :

```
CMake Error at CMakeLists.txt:75 (find_package):
  Could not find a package configuration file provided by "onnxruntime"
```

### Contexte
- Sur Windows : ONNX Runtime fourni par vcpkg (`onnxruntime-gpu`)
- Sur Jetson ARM64 : pas de paquet apt officiel, pas dans vcpkg pour ARM
- JetPack 6.2 inclut TensorRT 10.3 mais **pas ONNX Runtime** par défaut

### Solutions possibles (à choisir au moment du fix)

**Option A — Binaires NVIDIA pré-compilés pour Jetson (recommandé)**
NVIDIA fournit des wheels Python ET des binaires C++ ONNX Runtime optimisés pour Jetson :
https://elinux.org/Jetson_Zoo#ONNX_Runtime

Ajouter dans `base.Dockerfile` :
```dockerfile
RUN wget https://nvidia.box.com/.../onnxruntime-linux-aarch64-X.Y.Z.tgz && \
    tar -xzf onnxruntime-linux-aarch64-*.tgz -C /usr/local --strip-components=1 && \
    ldconfig
```

**Option B — Build from source dans le Dockerfile**
Long (~1-2h sur Jetson) mais 100% reproductible. Stage dédié dans le multi-stage.

**Option C — Refactor C++ : utiliser TensorRT directement**
Court-circuiter ONNX Runtime, charger le `.engine` TRT directement via l'API C++ TensorRT (déjà disponible via JetPack). C'est ce qui est prévu en Phase 2/3 de la migration.

### Solution provisoire (Phase 1a)
Aucune — on attend de voir l'erreur exacte au premier build pour choisir. Le `|| true` actuel laisse le build aller jusqu'à l'erreur cmake explicite.

### Notes / prévention
- À régler en Phase 1b dès le premier retour de build du Jetson.
- Documenter le choix de version (compatibilité TRT 10.3 + CUDA 12.6) avant download.
