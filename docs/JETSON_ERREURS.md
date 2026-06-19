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
| 48 | 2026-06-19 | Application.cpp — sélection PCB Map | ✅ RÉSOLU | [Clic sur la PCB Map ne sélectionne pas le composant visé : recherche par centre le plus proche (`c.position`) peu fiable sur carte dense → hit-test bbox](#erreur-48--clic-pcb-map-ne-selectionne-pas-le-bon-composant-nearest-center-peu-fiable) |
| 47 | 2026-06-19 | TrackingWorker.cpp — Live Tracking | ✅ RÉSOLU | [Overlay "vibre" pixel par pixel en Live Tracking sur scène statique — homographie refaite de zéro chaque frame sans lissage temporel](#erreur-47--overlay-vibre-en-live-tracking-sur-scene-statique-pas-de-lissage) |
| 46 | 2026-06-19 | Application.cpp — overlay caméra | ✅ RÉSOLU | [« Reset Alignment ne fait rien » : l'overlay est dessiné seulement si `m_homography->isValid()`, donc quand l'homographie devient invalide le bloc est sauté et la dernière image d'overlay reste figée à l'écran (jamais effacée)](#erreur-46--reset-alignment-ne-fait-rien-overlay-fige) |
| 45 | 2026-06-19 | IBomParser.cpp — détection pin 1 | ✅ RÉSOLU | [`pin1` lu uniquement comme booléen alors que l'iBOM l'encode en entier → pin 1 jamais détectée pour les parts dont le pad pin 1 n'est pas nommé "1" (ex. ESP32 U7)](#erreur-45--pin1-ibom-entier-non-detecte) |
| 44 | 2026-06-19 | BoardLocator.cpp — Auto-Align | ✅ RÉSOLU | [Auto-Align via profondeur "réussit" à score faible (0.13) sur carte coplanaire → overlay décalé ; la feuille blanche sous la carte ne servait à rien car le contour 2D n'était jamais essayé](#erreur-44--auto-align-depth-faible-score-contour-jamais-essaye) |
| 43 | 2026-06-19 | Application — sortie process | 🔴 OUVERT | [Segmentation fault au moment de quitter l'app (après "Application exiting with code 0") — non investigué](#erreur-43--segfault-a-la-sortie-de-lapplication) |
| 42 | 2026-06-19 | Application.cpp / BoardMinimap | ✅ RÉSOLU | [Clic minimap déplaçait tout l'overlay sur D405 (anchor 1-point pensé pour microscope FOV étroit) au lieu de surligner le composant](#erreur-42--clic-minimap-deplace-loverlay-sur-realsense-au-lieu-de-highlighter) |
| 41 | 2026-06-18 | BoardLocator.cpp — Auto-Align depth | 🟡 CONTOURNÉ | [Auto-Align D405 intermittent : carte posée à plat sur une surface coplanaire → le plan de profondeur englobe carte + table (2.5×), rejet par `validateSize()`](#erreur-41--auto-align-d405-carte-coplanaire-avec-la-table) |
| 40 | 2026-06-18 | StatsPanel.cpp — Inspection Progress | 🔴 OUVERT | [`StatsPanel::setTotalComponents()` jamais appelé — le panneau Inspection Progress affiche toujours "No inspection data" / 0%](#erreur-40--settotalcomponents-jamais-appele--inspection-progress-toujours-a-zero) |
| 39 | 2026-06-18 | BoardLocator.cpp / Application.cpp / RealSenseCapture.cpp / StatsPanel.cpp — D405 glare | ✅ RÉSOLU | [Distance/Auto-Align/self-cal faux sous glare D405 — depth fill bas non détecté](#erreur-39--distanceauto-alignself-cal-faux-sous-glare-d405) |
| 38 | 2026-06-18 | Application.cpp / BoardLocator — Auto-Align D405 | ✅ RÉSOLU | [Auto-Align échoue sur D405 : scale px/mm périmé (calibration checkerboard à une autre distance/caméra) rejette le bon contour](#erreur-38--auto-align-echoue-sur-d405-scale-pxmm-perime) |
| 37 | 2026-06-18 | Application.cpp / build Jetson | ✅ RÉSOLU | [Build Jetson échoue : variable locale `tr` masque `QObject::tr()` dans `autoAlignBoard()`](#erreur-37--variable-locale-tr-masque-qobjecttr-dans-autoalignboard) |
| 36 | 2026-06-18 | BoardLocator / Application — Auto-Align | ✅ RÉSOLU | [Audit Auto-Align : projet périmé dans le callback, pas de seuil de score, pas de filtre de couche, race avec alignement manuel](#erreur-36--audit-auto-align-projet-perime-pas-de-seuil-pas-de-filtre-de-couche-race) |
| 35 | 2026-06-18 | TrackingWorker / ORB | ✅ RÉSOLU | [ORB tracking se verrouille sur le fond statique au lieu de la carte déplacée à la main — pas de masque de détection sur la zone carte](#erreur-35--orb-tracking-verrouille-sur-le-fond-statique-au-lieu-de-la-carte) |
| 34 | 2026-06-17 | SettingsDialog / device combo | ✅ RÉSOLU | [Settings → Camera affiche "No camera detected" alors que la D405 streame — `enumerateCameras()` n'avait pas le même garde-fou que `Application::refreshCameraDeviceList()`](#erreur-34--settings-no-camera-detected-sur-d405-active) |
| 33 | 2026-06-17 | IBomParser / minimap bbox | ✅ RÉSOLU | [Bounding boxes composants décalées/superposées sur la minimap — `bbox.pos` lu comme coin, `relpos`/`angle` iBOM ignorés](#erreur-33--bbox-composants-decalees-relposangle-ignores) |
| 32 | 2026-06-17 | Application / device combo | ✅ RÉSOLU | [Combo Device montre le microscope alors que la D405 est active — énumération RealSense vide (device busy) → ancienne liste V4L2 conservée](#erreur-32--combo-device-montre-le-mauvais-backend) |
| 31 | 2026-06-17 | CameraCapture / GStreamer + index | 🟡 CONTOURNÉ | [Microscope inouvrable : index `/dev/video` instable (6→0) + pipeline GStreamer HW « ouvert » sans EGL ne produit aucune frame](#erreur-31--microscope-inouvrable-index-instable--gstreamer-sans-egl) |
| 30 | 2026-06-17 | RealSenseCapture / self-cal | 🟡 CONTOURNÉ | [Self-calibration D405 `hwmon ... -7 HW not ready` — profil depth 256×144@90 requis par le firmware (PAS l'USB2)](#erreur-30--self-calibration-d405-hw-not-ready) |
| 29 | 2026-06-17 | CameraCapture / V4L2 bandwidth | 🟡 CONTOURNÉ | [Microscope `select() timeout` (aucune frame) — YUYV 1280×720 négocié au lieu de MJPG sature l'USB 2.0](#erreur-29--microscope-select-timeout--yuyv-sature-lusb-20) |
| 28 | 2026-06-17 | Application / RealSense switch + USB | 🟡 CONTOURNÉ | [Freeze GUI au switch caméra + disparition de tout l'USB (`lsusb` = root hubs) — `query_devices()` sur le thread GUI pendant le hot-swap + reset xHCI Tegra](#erreur-28--freeze-au-switch-camera--disparition-usb) |
| 27 | 2026-06-16 | Application.cpp / calibration | ✅ RÉSOLU | [Calibration de mauvaise qualité (RMS 11 px) acceptée et sauvegardée — écrase une bonne calibration + corrompt l'overlay](#erreur-27--calibration-de-mauvaise-qualite-rms-11-px-acceptee) |
| 26 | 2026-06-16 | Application.cpp / Config | ✅ RÉSOLU | [Scale microscope faux (3.47 px/mm) — `opticalMultiplier` 0.3 multiplié par-dessus la calibration checkerboard (double comptage optique)](#erreur-26--scale-microscope-faux--double-comptage-optique) |
| 25 | 2026-06-16 | Application.cpp / StatsPanel | ✅ RÉSOLU | [Distance / Depth fill périmés affichés pour le microscope (stats depth D405 jamais réinitialisées)](#erreur-25--distance--depth-fill-perimes-affiches-pour-le-microscope) |
| 24 | 2026-06-16 | Application.cpp / RealSense | ✅ RÉSOLU | [D405 « Factory fx=0.0 px » — `updateCalibrationUI()` lit `colorFx()` avant la mise en cache des intrinsics](#erreur-24--d405-factory-fx00-px--intrinsics-lues-trop-tot) |
| 23 | 2026-06-16 | CameraCapture.cpp / V4L2 enum | ✅ RÉSOLU | [Liste caméras du profil Microscope inclut les nœuds UVC du D405 (RealSense vu deux fois)](#erreur-23--liste-cameras-du-profil-microscope-inclut-les-noeuds-uvc-du-d405) |
| 22 | 2026-06-16 | CameraCapture / GUI device combo | ✅ RÉSOLU | [Microscope inaccessible via l'UI — index combo (position) confondu avec l'index `/dev/video` réel (caméra à video6)](#erreur-22--index-combo-confondu-avec-lindex-devvideo-reel) |
| 21 | 2026-06-16 | CameraCapture.cpp / V4L2 | ✅ RÉSOLU | [`terminate called without an active exception` (SIGABRT) au switch caméra/profil — `std::thread` joignable détruit après auto-exit du captureLoop](#erreur-21--terminate-called-without-an-active-exception-au-switch-camera) |
| 20 | 2026-06-16 | PointCloudView.cpp / GUI | ✅ RÉSOLU | [`initializeFunctions was not declared` — faute de frappe pour `initializeOpenGLFunctions`](#erreur-20--initializefunctions-was-not-declared) |
| 19 | 2026-06-15 | SettingsDialog.h / GUI | ✅ RÉSOLU | [`SettingsDialog::accept() is private` — override sous `private slots:` appelé par MainWindow](#erreur-19--settingsdialogaccept-is-private) |
| 18 | 2026-06-14 | CameraCalibration.cpp | 🟡 EN COURS | [Calibration échoue `No checkerboard patterns detected` sur damier 7×5 valide — détecteur legacy capricieux](#erreur-18--calibration-echoue-sur-damier-valide--detecteur-legacy) |
| 17 | 2026-06-14 | Application.cpp / caméra | ✅ RÉSOLU | [`Found 0 camera(s)` sur device V4L2 fonctionnel — énumération via QMediaDevices au lieu d'OpenCV](#erreur-17--found-0-cameras-sur-device-v4l2-fonctionnel--enumeration-via-qmediadevices) |
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

## ERREUR 29 — Microscope `select() timeout` : YUYV sature l'USB 2.0

**Date** : 2026-06-17
**Statut** : 🟡 CONTOURNÉ (affichage du type USB + MJPG re-forcé après résolution ; à valider sur Jetson)

**Symptôme** (logs Jetson) :
```
Camera opened: 1280x720 @ 30 fps, FOURCC=YUYV ...
Camera warmup OK after 1 attempt(s)
[ WARN] global cap_v4l.cpp:1136 tryIoctl VIDEOIO(V4L2:/dev/video6): select() timeout.
```
Une seule frame de warmup, puis `select() timeout` toutes les ~10 s (le timeout select() d'OpenCV) → plus aucune frame.

**Cause** : la caméra a négocié **YUYV** (non compressé) et **non MJPG**, malgré `cap.set(CAP_PROP_FOURCC, MJPG)` avant la résolution. YUYV 1280×720@30 = 1280·720·2·30 ≈ **442 Mbit/s**, ce qui dépasse le débit utile pratique de l'**USB 2.0** (~300 Mbit/s). Le microscope est une caméra USB 2.0 (même branché sur un hub USB 3.2 Gen1, il négocie à 480 Mbit/s sur les lignes USB 2.0 du hub) → la bande passante est saturée → famine de frames.

**Pistes de contournement** :
1. **Affichage du type USB ajouté** (cette session) : `CameraCapture::listDevices()` lit la vitesse négociée dans sysfs (`/sys/class/video4linux/videoN/device` → remonte au nœud USB → `speed`) et l'affiche dans le combo (« USB 2.0 HS (480 Mb/s) »). Idem D405 via `RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR`. L'utilisateur voit ainsi pourquoi le microscope sature.
2. **Réduire la résolution YUYV** : 640×480 YUYV ≈ 147 Mbit/s passe en USB 2.0.
3. **Forcer réellement MJPG** ✅ (suite 51 + 54) : plusieurs drivers UVC / le backend V4L2 d'OpenCV resettent le pixelformat en YUYV **quand on change la résolution**, annulant le `CAP_PROP_FOURCC` posé avant. Fix initial (suite 51) : re-poser MJPG **après** `CAP_PROP_FRAME_WIDTH/HEIGHT/FPS`. **Mais la HAYEAR ignore quand même CAP_PROP_FOURCC** (log suite 54 : toujours `FOURCC=YUYV`). Fix robuste (suite 54) : si après ouverture V4L2 le FOURCC n'est pas MJPG, **ré-ouverture via une pipeline GStreamer CPU** `v4l2src ! image/jpeg ! jpegdec ! videoconvert ! appsink` (`buildGstPipelineCpu()`). Les caps explicites `image/jpeg` **forcent** la négociation MJPG côté driver, et `jpegdec` décode en CPU (pas de NVDEC/nvvidconv → **pas besoin d'EGL**, marche en container headless). Validation par lecture réelle d'une frame avant adoption, sinon on garde le flux brut.

> ⚠️ **Piège device index** (log suite 54) : l'app a ouvert `/dev/video6` (depuis `config.json`) alors que la HAYEAR expose AUSSI `/dev/video0` (le combo montrait « 0: HAYEAR_CAMERA »). `video6` est un **nœud secondaire** qui ne sort que du YUYV / pas de frames continues. **Action utilisateur** : sélectionner Device **0** + « Apply Camera » (ou corriger `camera_index` dans `config.json`). Le fallback par nom (#31) ne se déclenche pas ici car `video6` *s'ouvre* (il échoue juste à streamer).

**À valider** : prochain build — vérifier que le log microscope affiche `FOURCC=MJPG` ou « re-opened with CPU MJPG GStreamer pipeline » (plus de `select() timeout`).

---

## ERREUR 33 — bbox composants décalées (relpos/angle ignorés)

**Date** : 2026-06-17
**Statut** : ✅ RÉSOLU

**Symptôme** : sur la minimap PCB, les bounding boxes des composants sont décalées et se chevauchent (signalé par l'utilisateur, screenshot).

**Cause** : `IBomParser` lisait `footprint.bbox.pos` comme le **coin** de la boîte (`minX = pos.x; maxX = pos.x + size.x`). Or iBOM dessine la bbox ainsi : `translate(pos) → rotate(-angle) → translate(relpos) → rect(0,0,size)`. Donc `pos` est le **point de référence** du footprint, `relpos` l'offset vers le coin local, et la boîte est **tournée** de `angle`. En ignorant `relpos` (≈ `-size/2`) et `angle`, chaque boîte était décalée d'environ une demi-taille et mal orientée → positions fausses + chevauchements.

**Solution** : reconstruire les **4 coins tournés** (`pos + R(-angle)·(relpos + coin·size)`) et prendre leurs vraies bornes axis-aligned. `relpos` défaut `{0,0}`, `angle` défaut `0`. Fichier : `src/ibom/IBomParser.cpp` (+ `#include <cmath>`).

---

## ERREUR 32 — combo Device montre le mauvais backend

**Date** : 2026-06-17
**Statut** : ✅ RÉSOLU

**Symptôme** : la D405 est active (feed depth visible, toolbar « D405 »), mais le combo Device à droite montre encore « 0: HAYEAR_CAMERA » (le microscope V4L2).

**Cause** : `refreshCameraDeviceList()` interroge `RealSenseCapture::listDevices()` pendant que la D405 stream → lève « failed to set power state » → liste vide. Le garde anti-flicker (« ne pas écraser une liste peuplée quand une caméra est live ») **conservait alors la liste V4L2** du backend précédent.

**Solution** : dans la branche « énumération vide pendant capture », si le backend actif est RealSense, **synthétiser une entrée** « 0: Intel RealSense (active) » au lieu de garder la liste microscope. Le cas V4L2 (QUERYCAP en O_RDONLY marche même en streaming) garde l'ancien comportement. Fichier : `src/app/Application.cpp`.

---

## ERREUR 31 — Microscope inouvrable : index instable + GStreamer sans EGL

**Date** : 2026-06-17
**Statut** : 🟡 CONTOURNÉ (fallback par nom + validation du pipeline GStreamer)

**Symptômes** (logs Jetson) :
```
[error] Failed to open camera device 6 with any backend
```
puis, en activant le décodage HW :
```
No EGL Display
nvbufsurftransform: Could not get EGL display connection
[info] Camera opened with GStreamer (nvv4l2decoder, HW MJPG decode)
[info] Camera opened: -1x-1 @ 0 fps, FOURCC= (unified memory: yes)
```

**Causes (deux problèmes distincts)** :
1. **Index `/dev/video` instable** : après une ré-énumération USB (le collapse du bus au switch de backend, [ERREUR 28](#erreur-28--freeze-au-switch-camera--disparition-usb)), le microscope HAYEAR est passé de **`/dev/video6` à `/dev/video0`**. Le `config.json` pointe encore sur 6 → `cap.open(6)` échoue. (L'utilisateur l'a repéré : combo « 0: HAYEAR_CAMERA: MOS-4K Pro — USB 2.0 HS ».)
2. **Pipeline GStreamer HW « ouvert » mais mort** : `nvvidconv` a besoin d'un **display EGL**, absent dans un container headless → « No EGL Display ». La négociation des caps s'effondre (width/height = -1), **aucune frame ne circule**, mais `cap.isOpened()` renvoie quand même `true`. Le code acceptait ce pipeline mort, sautait le fallback CPU V4L2, puis le warmup échouait.

**Solution (suite 52)** :
1. **Fallback par nom** (`CameraCapture::captureLoop`, Linux) : si l'index configuré échoue à l'ouverture, scanne `listDevices()` et ouvre la **première caméra capture non-RealSense** détectée, en adoptant son index réel (`m_deviceIndex = idx`). Couvre le décalage 6→0.
2. **Validation du pipeline GStreamer** : après `cap.open(pipeline)`, on **lit réellement une frame** (jusqu'à 10 essais × 50 ms) avant de faire confiance à `isOpened()`. Si rien ne sort (cas EGL), on `release()` et on retombe sur le path CPU V4L2.

**Note EGL** : `nvvidconv` a besoin d'un display EGL. La cause partielle a été trouvée (suite 53) : le commit `64a3b13` avait **commenté tout le bloc `devices:` de `compose.yml`**, dont `/dev/dri` (moteur d'affichage Tegra, nécessaire à EGL), pour ne pas bloquer le démarrage sans caméra — mais `/dev/dri` est **toujours présent** et n'aurait pas dû être groupé avec les `/dev/video*`. C'est pourquoi le HW decode marchait « dans les anciennes versions ». **Fix (suite 53)** : `/dev/dri` remonté par défaut dans `compose.yml` (dev + runtime), retiré de `compose.local.yml` pour éviter le doublon (listes `devices` concaténées, [#14](#erreur-14--group_add-dupliques-par-le-merge-composeyml--composelocalyml)). Les `/dev/video*` restent opt-in/dynamiques. ⚠️ Si l'utilisateur lance via `run_local_gui.sh` (qui utilise déjà `compose.local.yml` montant `/dev/dri`), `/dev/dri` était **déjà** présent → si `No EGL Display` persiste, il manque les nœuds multimédia Tegra (`/dev/nvhost-*`, `/dev/nvmap`, injectés normalement par le nvidia-container-toolkit en mode CSV) — sujet toolkit/L4T à part. **Recommandation** : le path **CPU MJPG** suffit largement pour le microscope USB 2.0 (MJPG 1080p compressé tient dans 480 Mb/s) → laisser le HW decode **désactivé** sauf besoin avéré.

**À valider** : prochain build — au démarrage, le microscope s'ouvre même si le config a un vieil index ; HW decode activé → fallback propre sur CPU (plus de `-1x-1 @ 0 fps`).

---

## ERREUR 30 — Self-calibration D405 `HW not ready`

**Date** : 2026-06-17
**Statut** : 🟡 CONTOURNÉ (garde-fous + retry ; le succès dépend de l'état USB/firmware)

**Symptôme** (log Jetson, bouton « Self-calibration depth (sans mire)… » du panneau RealSense Controls) :
```
[warning] RealSense on-chip calibration failed: hwmon command 0x80( 8 3 0 8 ) failed (response -7= HW not ready)
```

**Cause** : erreur **firmware** (pas applicative) renvoyée par `rs2::auto_calibrated_device::run_on_chip_calibration()`. Le D4xx refuse la commande hwmon quand il n'est pas dans un état stable : (a) routine lancée **trop tôt** après l'ouverture / un switch de backend (streams pas encore stabilisés) ; (b) lien **USB 2.x** dégradé (la self-cal stéréo demande de la bande passante/puissance) ; (c) flux depth désactivé.

**Solution (garde-fous, suite 51)** :
- Côté capture (`RealSenseCapture.cpp`, bloc on-chip calib) : refuse si `!m_depthStreamEnabled` ou si aucun flux (`colorFps`/`depthFps` ≤ 0) ; warning si lien USB 2.x (`RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR`) ; **retry ×3 avec pause 800 ms** sur l'erreur transitoire ; message d'aide enrichi (USB3 + immobile + surface texturée) en cas d'échec « HW not ready ».
- Côté UI (`RealSenseControlsDialog.cpp`) : pré-checks avant d'envoyer la requête (caméra qui diffuse + depth actif) avec QMessageBox clair, et mention USB3 dans la confirmation.

**Update suite 55 — VRAIE CAUSE TROUVÉE (recherche web, pas USB2)** : l'utilisateur a confirmé que la D405 est en **USB 3.2** → l'hypothèse USB2 était fausse. Les 3 tentatives échouaient toutes alors que la D405 streame bien (fx=436.8, distance 188mm, depth fill 84%). **Cause réelle** : `run_on_chip_calibration()` exige que le **flux depth tourne au profil 256×144 @ 90 fps** au moment de l'appel — c'est une **contrainte firmware D4xx**. Avec n'importe quel autre profil depth (on streame normalement en 848×480 / 1280×720), le firmware rejette la commande hwmon avec `-7 = HW not ready`. Confirmé par les issues librealsense [#7087](https://github.com/IntelRealSense/librealsense/issues/7087) (code C++ : `cfg.enable_stream(RS2_STREAM_DEPTH, 256, 144, RS2_FORMAT_Z16, 90)` avant l'appel) et [#12014](https://github.com/IntelRealSense/librealsense/issues/12014) (marche via le Viewer GUI — qui bascule sur ce profil — mais pas en script qui garde la pleine résolution). **Fix (suite 55)** : dans le bloc on-chip calib (`RealSenseCapture.cpp`), on **stoppe le pipeline normal**, on redémarre **depth-only en 256×144@90** (lié au même device par serial), on laisse le flux se stabiliser, on lance la calibration (retry ×3 conservé pour le résiduel transitoire), puis on **restaure le pipeline de streaming normal** (color+depth+IR à la résolution courante) dans tous les cas (succès/échec/exception), avec re-publication du device + depth_units. ⚠️ Non compilé/testé ici. **À valider** : prochain build — la self-cal doit réussir (log « calibration done, health=… ») ou afficher un message clair, et le streaming normal reprendre après. La **calibration usine reste valide** de toute façon → feature optionnelle.

---

## ERREUR 28 — Freeze au switch caméra + disparition USB

**Date** : 2026-06-17
**Statut** : 🟡 CONTOURNÉ (freeze GUI atténué côté logiciel ; collapse USB = matériel/kernel)

**Symptôme** : au passage profil Microscope (V4L2) → D405 (RealSense), l'appli **freeze**. Les logs s'arrêtent net après « Creating RealSenseCapture (D405)... » (rien après). Puis, de retour sur l'hôte, **tout l'USB a disparu** :
```
lololo@jetson:~$ lsusb
Bus 002 Device 001: ID 1d6b:0003 Linux Foundation 3.0 root hub
Bus 001 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub
```
(plus aucun device — seulement les root hubs).

**Causes** (deux problèmes distincts) :
1. **Freeze GUI (logiciel)** : `switchCameraBackend()` tourne sur le thread GUI et appelait `refreshCameraDeviceList()` → `RealSenseCapture::listDevices()` → `rs2::context::query_devices()` **de façon synchrone**, juste après avoir relâché le microscope défaillant. `query_devices()` énumère tout l'arbre USB et **bloque le thread GUI**. Pire : le thread de capture (`captureLoop`) énumère **lui aussi** (`query_devices()`) quelques ms plus tard → l'USB est sondé **deux fois** au moment le plus fragile.
2. **Disparition de tout l'USB (matériel/kernel)** : c'est le contrôleur **xHCI Tegra qui se reset et largue tous les devices**. Sur AGX Orin c'est quasi toujours un **reset de bus / budget d'alimentation** : monter le lien USB3 de la D405 (renégociation + ~2 W) au moment précis où une caméra USB2 défaillante est relâchée, sur le même contrôleur. **Aucun code applicatif ne peut empêcher de façon fiable un reset du contrôleur xHCI.** Un hub alimenté règle le *budget de puissance* mais **pas forcément** le reset du contrôleur (le lien amont du hub vers le Jetson peut se reset quand la D405 renégocie l'USB3).

**Fix logiciel (cette session, atténuation du freeze)** :
- `switchCameraBackend()` : suppression de l'appel synchrone à `refreshCameraDeviceList()` pendant le hot-swap. Remplacé par un `QTimer::singleShot(1500, …)` → l'énumération est différée hors de l'instant fragile (bus stabilisé, pipeline déjà en stream, une seule énumération au lieu de deux).
- `refreshCameraDeviceList()` : garde « ne pas écraser » — si l'énumération revient vide **alors qu'une caméra est en cours de capture** (cas RealSense occupée → `query_devices` jette « failed to set power state »), on **conserve la liste existante** au lieu de basculer le combo en « No camera detected ».

**Côté matériel (à faire par l'utilisateur)** :
- Récupérer le bus sans reboot : `unbind`/`bind` du driver `xhci_hcd` via `/sys/bus/pci/drivers/xhci_hcd/` (ou power-cycle).
- Les deux caméras sont déjà sur un hub alimenté **USB 3.2 Gen1** — bien pour la puissance. Si le collapse persiste, c'est un **reset de contrôleur**, pas un manque de jus : capturer `dmesg` juste après pour confirmer (chercher `xhci`, `USB disconnect`, `over-current`). Pistes : brancher la D405 sur un **contrôleur xHCI distinct** du microscope, ou désactiver l'autosuspend USB.

**À valider** : prochain build — vérifier que le switch ne freeze plus (surtout microscope sans frame → D405) ; capturer `dmesg` si l'USB retombe pour confirmer la nature du reset xHCI.

---

## ERREUR 27 — Calibration de mauvaise qualité (RMS 11 px) acceptée

**Date :** 2026-06-16
**Composant :** Application::runCalibration
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) suite 46

### Symptôme
Une calibration microscope renvoie `pixels/mm=11.56` mais **RMS error=11.09** (une bonne calibration = < 1 px ; la précédente était à 0.876). L'app affiche « Calibration succeeded », sauvegarde, et applique — l'overlay/undistort devient faux (« calibration ne fonctionne plus »). La bonne calibration précédente sur disque est écrasée.

### Cause
`runCalibration()` ne testait que `error < 0` (= damier non détecté). Tout solve « réussi » était accepté, **quelle que soit la qualité**. Un RMS de 11 px signifie un modèle d'intrinsics incohérent — typiquement : damier flou, angle trop prononcé, ou **trop peu de variation de pose** entre les 5 prises (fréquent avec le champ étroit du microscope : peu de place pour incliner). Le `pixels/mm` quasi identique (11.56 vs 11.58) confirme que la détection de coins était bonne (échelle correcte) mais que le bundle adjustment multi-images divergeait.

### Solution appliquée ✅
Gate qualité dans `runCalibration()` après le solve : si `error > 1.5 px`, avertir (QMessageBox, défaut **No**) et — si l'utilisateur ne force pas — **ne pas** sauvegarder/appliquer ; recharger la calibration précédente depuis le disque (`load()` + `initUndistortMaps`) pour ne pas corrompre l'existant. Le chemin `calibPath` est calculé en amont pour permettre ce rollback. Message d'aide explicite (incliner le damier à un angle différent à chaque prise, rester net).

### Leçon
Un code de retour « réussi » d'un solveur ne garantit pas la **qualité** du résultat. Pour la calibration, le RMS de reprojection est le critère de qualité : toujours le borner avant de remplacer une calibration existante. Et au microscope (champ étroit), varier l'angle entre les prises est crucial — sinon le solve dégénère.

---

## ERREUR 26 — Scale microscope faux — double comptage optique

**Date :** 2026-06-16
**Composant :** Application (calibration-complete + settings-apply) / Config (profil Microscope)
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) suite 45

### Symptôme
Après une calibration checkerboard réussie du microscope (« Pixels per mm: 11.58 »), le panneau Statistics affiche « Scale: 3.5 px/mm ». 3.5 = 11.58 × 0.3 — l'overlay et les mesures utilisent donc une échelle physiquement fausse.

### Contexte optique (réel)
Microscope = caméra + **0.35× (adaptateur sous la caméra)** + **0.7× (lentille du microscope)** + bague d'éclairage polarisée (sans effet sur l'échelle). Le profil Microscope portait `opticalMultiplier = 0.3` (approximation de la réduction optique).

### Cause
Le `opticalMultiplier` était multiplié **par-dessus** le résultat de la calibration. Or la calibration checkerboard mesure le px/mm à travers **toute** la chaîne optique (capteur + 0.35× + 0.7×) — c'est déjà l'échelle effective réelle. La remultiplier par 0.3 double-compte l'optique → 3.47, qui ne correspond à rien. Les chemins homography/depth, eux, fixaient déjà `m_currentPixelsPerMm` **sans** multiplier → incohérence : seule la voie calibration était biaisée.

### Solution appliquée ✅ (calibration autoritaire)
1. Calibration terminée (`onCalibrationComplete`) : `m_currentPixelsPerMm = m_basePixelsPerMm` (suppression de `* opticalMultiplier()`).
2. Handler settings-apply : multiplier appliqué **uniquement si la caméra n'est pas calibrée** (`!m_calibration->isCalibrated()`). Une vraie calibration prime toujours.
3. Défaut du profil Microscope : `opticalMultiplier` 0.3 → **1.0** (neutre).
Résultat : calibration / homography / depth donnent tous l'échelle vraie ; le multiplier n'est plus qu'un réglage manuel pour le cas **non calibré**.

### Leçon
Une calibration (checkerboard, ou depth `fx/distance`, ou homographie sur pads iBOM) capture **déjà** l'effet de toute l'optique physique. Ne jamais ré-appliquer un facteur optique « nominal » par-dessus une mesure réelle — c'est un double comptage. Les facteurs de lentille (0.35×, 0.7×…) n'ont pas à être saisis : ils sont mesurés par la calibration.

---

## ERREUR 25 — Distance / Depth fill périmés affichés pour le microscope

**Date :** 2026-06-16
**Composant :** Application::updateCalibrationUI + StatsPanel
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) suite 45

### Symptôme
En profil Microscope (V4L2, sans capteur de profondeur), le panneau Statistics affiche encore « Distance: 190.0 mm » et « Depth fill: 77% » — des valeurs héritées de la session D405 précédente.

### Cause
`Distance` et `Depth fill` ne sont mis à jour que par le handler `depthFrameReady` (RealSense uniquement). Au passage en V4L2, ce signal ne se déclenche plus jamais → les labels gardent leur dernière valeur D405. Aucun code ne les réinitialise au changement de backend.

### Solution appliquée ✅
Dans `Application::updateCalibrationUI()`, branche V4L2/microscope (backend sans depth), appeler `sp->setDistance(-1.0)` et `sp->setFillRate(-1.0)` → affiche « — » (sentinelles déjà gérées par `StatsPanel::setDistance(<=0)` / `setFillRate(<0)`). `updateCalibrationUI()` est déjà appelée à chaque changement de backend et en fin de calibration.

### Leçon
Toute statistique alimentée par un seul backend (depth, IR, etc.) doit être explicitement remise à « indisponible » quand on bascule vers un backend qui ne la produit pas — sinon l'UI ment avec des valeurs périmées.

---

## ERREUR 24 — D405 « Factory fx=0.0 px » — intrinsics lues trop tôt

**Date :** 2026-06-16
**Composant :** Application::updateCalibrationUI / RealSenseCapture::colorFx
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) suite 45

### Symptôme
Panneau Statistics en profil D405 : « Calibration: Factory fx=0.0 px », alors que les logs montrent `RealSense color opened: … fx=436.8px`. La FOV/intrinsics du tooltip restent vides.

### Cause
`RealSenseCapture` met en cache les intrinsics (`m_colorFx` …) dans `captureLoop()`, **sur le thread de capture**, juste avant la boucle de frames. Or `updateCalibrationUI()` est appelée **synchroniquement** depuis `wireCameraSignals()` juste après `start()` — à cet instant le thread de capture n'a pas encore atteint la mise en cache, donc `colorFx()` renvoie encore 0.0 (défaut). Le label se fige sur la première lecture.

### Solution appliquée ✅
Rafraîchir une fois le label dès que les intrinsics sont disponibles : dans le handler `frameReady` (qui tourne pour chaque backend), flag **one-shot par connexion** (`intrinsicsShown`, init-capture `mutable` → se réinitialise tout seul à chaque hot-swap de backend). Quand `backend == RealSense` et que `colorFx() > 0`, appeler `updateCalibrationUI()` une seule fois. Coût = un `dynamic_cast` sur les quelques premières frames jusqu'à ce que fx soit disponible, puis plus rien.

### Leçon
Les intrinsics RealSense ne sont valides qu'**après** que le pipeline a démarré et livré une frame. Toute lecture de `colorFx()`/`colorFy()`/… faite synchroniquement juste après `start()` voit 0. Lire ces valeurs dans un handler de frame (ou via un signal émis après la mise en cache), jamais en synchrone post-start.

---

## ERREUR 23 — Liste caméras du profil Microscope inclut les nœuds UVC du D405

**Date :** 2026-06-16
**Composant :** CameraCapture::listDevices (V4L2)
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) suite 44

### Symptôme
En profil Microscope (backend V4L2), le combo « Device » affiche en plus du microscope (`6: HAYEAR_CAMERA…`) trois entrées RealSense (`0/2/4: Intel(R) RealSense(TM) Depth…`), alors que ces caméras ne devraient apparaître que dans le combo du profil D405.

### Cause
Le D405 expose ses flux couleur/IR/depth comme de **vrais nodes UVC** `/dev/video*` en plus de l'API RealSense SDK. `VIDIOC_QUERYCAP` les rapporte donc comme caméras capture valides, et `CameraCapture::listDevices()` (corrigée en ERREUR 22 pour énumérer via `VIDIOC_QUERYCAP`) les inclut sans distinction. Sélectionner une de ces entrées en mode V4L2 ouvrirait un flux RealSense brut, non rectifié, sans aucun lien avec le pipeline RealSense SDK (factory calibration, depth align) — comportement cassé/trompeur.

### Solution appliquée ✅
Dans `CameraCapture::listDevices()`, filtrer (`continue`) tout node dont le nom de carte (`v4l2_capability.card`) contient `"RealSense"`. Le combo Microscope ne liste plus que les caméras UVC génériques (microscope).

### Leçon
Sur Jetson, une caméra RealSense occupe à la fois l'API librealsense **et** plusieurs nodes `/dev/video*` UVC bruts. Toute énumération V4L2 générique doit explicitement exclure les devices RealSense pour éviter de les exposer deux fois sous deux APIs différentes.

---

## ERREUR 22 — Index combo confondu avec l'index `/dev/video` réel

**Date :** 2026-06-16
**Composant :** CameraCapture::listDevices + Application/ControlPanel/SettingsDialog (combo device)
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) suite 43

### Symptôme
Aucun device caméra ne s'ouvre (« can't open camera by index » sur `/dev/video0`…`/dev/video9`), même en sélectionnant la caméra dans le menu déroulant. Dans l'énumération, **`video6` est absent** des warnings d'échec → c'est le seul node ouvrable (la vraie caméra USB).

```
Trying camera 0 with V4L2 backend ... can't open camera by index
... video0..video5, video7..video9 échouent (video6 absent → ouvrable)
Failed to open camera device 0 with any backend
```

### Contexte
- Jetson + D405 (occupe des nodes `/dev/video` bas) + microscope USB sur un node plus haut (video6).
- Le profil Microscope (index 0 par défaut) tape sur `/dev/video0` (un node D405 / non-capture) → échec.

### Cause
Triple confusion **position dans la liste** ↔ **index `/dev/video` réel** :
1. `CameraCapture::listDevices()` renvoyait `["Camera 6"]` (index réel noyé dans la string, perdu comme info positionnelle).
2. `refreshCameraDeviceList()` / `SettingsDialog::enumerateCameras()` étiquetaient avec la **position** (`arg(qi)`) au lieu de l'index réel, et stockaient la position comme `itemData`.
3. `ControlPanel::cameraIndex()` / `SettingsDialog::accept()` renvoyaient `combo->currentIndex()` (= **position** du combo) comme index de device.

→ Avec le microscope à video6 et un seul device listé, le combo affichait « 0: … » et sélectionner cette unique entrée appelait `setDeviceIndex(0)` → `/dev/video0` → échec. **Le microscope était littéralement inatteignable depuis l'UI.**

En prime : l'énumération via `cv::VideoCapture::open(i, CAP_ANY)` sur chaque index inondait le log de warnings GStreamer/obsensor.

### Solution appliquée ✅
1. `CameraCapture::listDevices()` → renvoie `std::vector<std::pair<int,std::string>>` (index `/dev/video` **réel** + nom). Énumération Linux via `::open` + `VIDIOC_QUERYCAP` directement : (a) donne l'index réel, (b) lit le **nom de la carte** (distingue microscope ↔ D405), (c) filtre les nodes non-capture (`V4L2_CAP_VIDEO_CAPTURE`), (d) supprime le spam de warnings OpenCV.
2. Combo : stocke l'index réel en `itemData` ; `setCameraDevices(labels, indices, currentIndex)` sélectionne via `findData`. `ControlPanel::cameraIndex()` et `SettingsDialog::accept()` lisent `currentData().toInt()`. Sélection au chargement par `findData` (jamais par position).

### Leçon
Sur Linux, les nodes `/dev/video` ont des **trous** (multi-node par device, nodes metadata, plusieurs caméras). Ne jamais assimiler « N-ième caméra ouvrable » à `/dev/video<N>` ni à la position dans un combo : transporter l'index réel de bout en bout (item data), et énumérer via `VIDIOC_QUERYCAP` pour ne garder que les nodes capture.

---

## ERREUR 21 — `terminate called without an active exception` (SIGABRT) au switch caméra

**Date :** 2026-06-16
**Composant :** CameraCapture.cpp / V4L2
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) suite 41

### Symptôme
Crash (abort) après avoir cyclé sur plusieurs devices caméra puis changé de profil/backend (ou à la fermeture). Le device 0 ne s'ouvrait pas (« available but can't be used to capture by index »).

```
[18:05:46.077] Camera settings applied: device=0 1920x1080 @30fps
terminate called without an active exception
[18:05:48.816] UNHANDLED SIGNAL 6 (Aborted)
  ...
  libstdc++ __verbose_terminate_handler
  QComboBox::currentIndexChanged(int)
  ...itemSelected (mouse event)
Aborted (core dumped)
```

### Contexte
- Combo profil (toolbar) ou combo device (ControlPanel) → `switchProfile`/`cameraSettingsChanged`.
- Un device V4L2 ne pouvait pas s'ouvrir → `captureLoop()` mettait `m_capturing=false` et sortait **tout seul**.
- Reproductible : oui, dès qu'un device échoue à l'ouverture puis qu'on détruit/relance la caméra.

### Cause
`m_capturing` servait à la fois de « doit continuer la boucle » **et** de proxy « le thread est vivant ». Quand `captureLoop()` s'auto-terminait (échec d'open), `m_capturing` passait à `false` mais l'objet `std::thread` restait **joignable** (fini mais jamais joint). Or `CameraCapture::stop()` faisait `if (!m_capturing.load()) return;` **avant** le join → au `m_camera.reset()` (switch profil) ou au destructeur, le `unique_ptr<std::thread>` détruisait un thread joignable → `std::terminate()` → SIGABRT. `RealSenseCapture::stop()` avait déjà été corrigé (entrée non loggée à l'époque), mais **pas** `CameraCapture::stop()`.

### Solution appliquée ✅
`CameraCapture::stop()` : ne plus early-return sur `!m_capturing` — toujours `join()` + `reset()` le thread s'il existe (via `m_capturing.exchange(false)` pour ne logger/émettre que si on était bien en train de capturer). Même logique que `RealSenseCapture::stop()`. En complément, `start()` (CameraCapture **et** RealSenseCapture) joint/reset un éventuel thread résiduel **avant** de réassigner `m_thread` (sinon la réassignation détruirait un thread joignable). Diff dans `src/camera/CameraCapture.cpp` + `src/camera/RealSenseCapture.cpp`.

### Leçon
Ne jamais utiliser un flag « en cours » comme proxy de « thread joignable ». Un `std::thread` doit être joint/détaché avant toute destruction ou réassignation, **indépendamment** de l'état logique de la boucle.

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

## ERREUR 20 — `initializeFunctions was not declared`

**Date :** 2026-06-16
**Composant :** GUI / PointCloudView.cpp
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-06-16

### Symptôme
Build Jetson échoue à la compilation de `PointCloudView.cpp` :

```
/opt/microscope-ibom/src/gui/PointCloudView.cpp:103:5: error: ‘initializeFunctions’ was not declared in this scope; did you mean ‘initializeOpenGLFunctions’?
  103 |     initializeFunctions();
      |     ^~~~~~~~~~~~~~~~~~~
      |     initializeOpenGLFunctions
```

### Contexte
- Commande lancée : `bash scripts/build_jetson.sh` (ou ninja dans `/opt/microscope-ibom/build`)
- Faute de frappe introduite à la création de `PointCloudView` (session 2026-06-15 suite 23). Jamais détectée avant car rien ne compilait ce fichier dans l'environnement d'analyse (pas de Qt/OpenGL) — seul un build Jetson l'attrape.
- Reproductible : oui (déterministe)

### Cause
`PointCloudView` hérite de `QOpenGLWidget, protected QOpenGLFunctions`. La méthode d'initialisation des pointeurs de fonctions GL fournie par `QOpenGLFunctions` s'appelle **`initializeOpenGLFunctions()`**, pas `initializeFunctions()`.

### Solution appliquée ✅
```diff
- initializeFunctions();
+ initializeOpenGLFunctions();
```

### Notes / prévention
- Rappel récurrent (cf. suites 18-19) : **la CI ne compile pas le C++**, seul un build Jetson attrape ce type d'erreur. Tout nouveau widget Qt/GL doit être validé par un build Jetson réel.
- Le `Wall -Wextra` n'aide pas ici : c'est une vraie erreur de nom non déclaré, pas un warning.

---

## ERREUR 19 — `SettingsDialog::accept() is private`

**Date :** 2026-06-15
**Composant :** SettingsDialog.h / GUI
**Statut :** ✅ RÉSOLU
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-06-15 (suite 18)

### Symptôme
Premier build Jetson de la branche `claude/fix-realsense-undistort` :

```
/opt/microscope-ibom/src/gui/MainWindow.cpp:383:19: error: 'virtual void SettingsDialog::accept()' is private within this context
  383 |         dlg.accept();
src/gui/SettingsDialog.h:29:10: note: declared private here
   29 |     void accept() override;
```

### Contexte
- Commande : `bash scripts/build_jetson.sh`
- Introduit en suite 12 (bouton « RealSense Controls » dans Settings) : `MainWindow::onShowSettings` appelle `dlg.accept()` pour fermer Settings avant d'ouvrir le panneau RealSense.
- Reproductible : oui (n'avait jamais été compilé — la CI ne build pas le C++).

### Cause
`QDialog::accept()` est un **slot public**. L'override dans `SettingsDialog` était déclaré sous `private slots:`, donc inaccessible depuis `MainWindow`. C++ interdit d'appeler une méthode override privée même si la méthode de base est publique (l'accès est déterminé par le type statique `SettingsDialog`).

### Solution
Déplacer la déclaration sous `public slots:` (cohérent avec `QDialog`).

```diff
- private slots:
-     void accept() override;
+ public slots:
+     void accept() override;
```

### Notes / prévention
La CI GitHub ne compile pas le C++ (pas de toolchain CUDA/Qt/OpenCV) → ce type d'erreur d'accès ne sera **jamais** attrapé en CI. Toujours valider un build Jetson avant merge. ⚠️ `main` a hérité du bug via la PR #7 ; il est corrigé par la PR #8.

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

## ERREUR 18 — Calibration échoue sur damier valide — détecteur legacy

**Date :** 2026-06-14
**Composant :** CameraCalibration.cpp / `cv::findChessboardCorners`
**Statut :** 🟡 EN COURS (fix appliqué, validation Jetson en attente)

### Symptôme
Caméra HAYEAR fonctionnelle, flux net et contrasté, damier **8×6 cases = 7×5 coins internes**
(= config par défaut `calibBoardCols=7`/`calibBoardRows=5`), entièrement dans le cadre. Les 5
images de calibration sont capturées mais : `No checkerboard patterns detected in calibration
images` → `Calibration failed`.

### Cause
`cv::findChessboardCorners` (détecteur **legacy**) est connu pour échouer fréquemment sur des
damiers pourtant valides dès qu'il y a perspective, éclairage non uniforme ou reflets — typique
d'une caméra microscope. La géométrie et la frame (brute, clonée depuis `latestFrame`) étaient
correctes ; ce n'était ni un problème de config, ni d'overlay.

### Solution
Basculer sur **`cv::findChessboardCornersSB`** (sector-based, OpenCV 4.x, sous-pixel natif,
nettement plus robuste) avec flags `NORMALIZE_IMAGE | EXHAUSTIVE | ACCURACY`, et **fallback**
sur le détecteur legacy + `cornerSubPix` si SB échoue. Ajout d'un `spdlog::warn` par image non
détectée pour diagnostiquer les échecs restants. Patch dans `CameraCalibration::calibrate()`.

> À valider sur Jetson : refaire les 5 prises et confirmer que la calibration aboutit (RMS
> error loggée + `calibration.yml` écrit). Passer ✅ RÉSOLU une fois confirmé.

---

## ERREUR 17 — `Found 0 camera(s)` sur device V4L2 fonctionnel — énumération via QMediaDevices

**Date :** 2026-06-14
**Composant :** Application.cpp / énumération caméra (Qt Multimedia vs OpenCV V4L2)
**Statut :** ✅ RÉSOLU (2026-06-14 — caméra HAYEAR 1920×1080 MJPG visible à l'écran)
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

## ERREUR 34 — Settings → Camera affiche "No camera detected" sur D405 active

**Date :** 2026-06-17
**Composant :** `src/gui/SettingsDialog.cpp` (`enumerateCameras()`)
**Statut :** ✅ RÉSOLU

### Symptôme
Caméra D405 active et streamant normalement (depth fill 83%, fx=436.8, calibration on-chip réussie dans le log) mais le dialogue Settings → onglet Camera affiche **"No camera detected"** dans le combo Device, en surbrillance jaune sur le screenshot utilisateur.

### Cause
`SettingsDialog::enumerateCameras()` a sa **propre** logique d'énumération RealSense (`RealSenseCapture::listDevices()` → `rs2::context ctx; ctx.query_devices()`), indépendante de celle d'`Application::refreshCameraDeviceList()`. Un `rs2::context` fraîchement créé peut ne voir aucun device si celui-ci est déjà exclusivement détenu par le pipeline de streaming actif du process (cas normal : Settings est ouvert pendant que la caméra tourne). `Application::refreshCameraDeviceList()` avait déjà ce garde-fou ([ERREUR #32](#erreur-32)), mais `SettingsDialog` ne le reprenait pas — d'où le "No camera detected" alors même que la D405 est visiblement active dans le viewport principal.

### Solution appliquée ✅
Dans `SettingsDialog::enumerateCameras()`, callback de fin d'énumération : si `realsense && names.isEmpty()`, synthétiser une entrée `"<previousIndex>: Intel RealSense (active)"` au lieu de tomber sur "No camera detected", en réutilisant l'index précédemment sélectionné (`previousIndex`, déjà tracké pour la ré-sélection après refresh). Lambda passée à `QMetaObject::invokeMethod` rendue `mutable` (capturait `names`/`indices` par valeur en `const` sinon) + capture de `realsense`.

### Leçon
Deux endroits enuméraient les caméras RealSense avec le même piège (`rs2::context` frais ≠ device visible si déjà ouvert ailleurs dans le process) mais un seul avait le garde-fou. Vérifier toutes les implémentations dupliquées d'une même logique d'énumération quand un bug de ce type est corrigé une fois.

## ERREUR 35 — ORB tracking verrouillé sur le fond statique au lieu de la carte

**Date :** 2026-06-18
**Composant :** `src/overlay/TrackingWorker.cpp` (`processFrame()`)
**Statut :** ✅ RÉSOLU

### Symptôme
Live Tracking Mode activé, alignement initial réussi (log : `Homography computed: 4/4 inliers, error: 0.000 px`, `Live tracking mode enabled`, `TrackingWorker: reference captured (179 keypoints)`), mais quand l'utilisateur prend la carte PCB dans la main et la déplace sous la caméra fixe, l'overlay reste figé à sa position d'origine au lieu de suivre le mouvement.

### Cause
`TrackingWorker::processFrame()` appelait `m_detector->detectAndCompute(small, cv::noArray(), kp, desc)` — ORB détecte des points-clés sur **toute l'image**, y compris le fond (table, câbles, accessoires) qui reste statique pendant que seule la carte bouge. `cv::findHomography` (RANSAC) retient le plus grand ensemble de correspondances internement cohérent : si le fond a une densité de points-clés comparable ou supérieure à celle de la carte, RANSAC verrouille sur le fond (transform quasi-identité) plutôt que sur le mouvement réel de la carte — d'où un overlay qui ne bouge jamais.

### Solution appliquée ✅
Masquage de la détection ORB à la zone de la carte :
- Nouveau slot `TrackingWorker::setBoardPolygon(std::vector<cv::Point2f> pcbPoints)` — reçoit les 4 coins du bbox de la carte en coordonnées PCB, appelé une fois depuis `Application::loadIBomFile()` après chargement d'un iBOM (couvre aussi bien l'ouverture manuelle que le rechargement automatique au démarrage). Persiste à travers `resetReference()`/re-ancrages.
- Nouvelle méthode privée `buildBoardMask(smallSize, downscale)` — projette le polygone via la dernière homographie connue (`m_lastHomography`, tenue à jour après chaque estimation réussie), l'agrandit ×1.6 depuis son centroïde (tolère le mouvement depuis la dernière estimation), remplit un masque `CV_8U` via `cv::fillConvexPoly`. Retourne un `Mat` vide si pas de polygone/homographie disponible → fallback détection non masquée (pas de régression avant le premier ancrage).
- `detectAndCompute()` reçoit désormais ce masque au lieu de `cv::noArray()`.
- Registration Qt cross-thread pour le nouveau type de paramètre : `Q_DECLARE_METATYPE(std::vector<cv::Point2f>)` + `qRegisterMetaType<std::vector<cv::Point2f>>("std::vector<cv::Point2f>")` dans `Application::initialize()` — chaîne exacte requise pour matcher le type qualifié du slot (CLAUDE.md piège #17).

### Leçon
Pour du tracking incrémental "objet tenu à la main sous caméra fixe", ne jamais laisser un détecteur de features tourner sans masque sur une scène contenant un fond statique potentiellement riche en texture — RANSAC n'a aucune notion sémantique de "l'objet d'intérêt", il optimise juste la cohérence du plus grand sous-ensemble.

## ERREUR 36 — Audit Auto-Align : projet périmé, pas de seuil, pas de filtre de couche, race

**Date :** 2026-06-18
**Composant :** `src/overlay/BoardLocator.{h,cpp}`, `src/app/Application.{h,cpp}`
**Statut :** ✅ RÉSOLU

### Symptôme
Pas un bug rapporté par l'utilisateur — trouvé par un audit demandé explicitement (« peux tu faire un audit pour trouver des bugs ») sur le diff Auto-Align (`git diff origin/main HEAD`, 9 fichiers/695 lignes), via une revue multi-angles (code-review skill).

### Causes et corrections
1. **Projet iBOM périmé** — le callback `finished` de la lambda Auto-Align lisait le membre live `m_ibomProject` (peut changer ou devenir nul pendant la détection sur thread worker, si l'utilisateur charge un autre iBOM en attendant) au lieu de la copie `project` capturée au moment du dispatch. → `pcbCorners` reconstruit depuis `project->boardInfo.boardBBox`, `project` ajouté à la capture lambda.
2. **`result.found` toujours vrai** — `BoardLocator::disambiguate()` n'appliquait aucun seuil minimal sur le score d'orientation ; un iBOM sans `boardOutline` ni composants sur la couche active fait que tous les 8 candidats scorent exactement 0.0, et le premier "gagne par défaut" (`bestScore` initialisé à -1.0) → reporté comme succès. → constante `kMinAcceptableScore = 0.10`, `result.found = !bestImgCorners.empty() && bestScore >= kMinAcceptableScore`.
3. **Filtre de couche manquant** — `scoreOrientation()` rendait tous les composants (front+back) alors qu'`OverlayRenderer` ne rend que `m_activeLayer` ; dilue le score sur une carte double-face. → paramètre `ibom::Layer activeLayer` propagé `locate()`→`disambiguate()`→`scoreOrientation()`, filtre `if (comp.layer != activeLayer) continue;`.
4. **Race avec un alignement manuel concurrent** — aucune notion d'ordre entre le résultat Auto-Align (arrivant après un délai sur thread worker) et un alignement manuel déclenché pendant ce délai ; le dernier à arriver écrasait l'autre silencieusement. → compteur `Application::m_alignmentEpoch`, incrémenté à chaque alignement appliqué, capturé au dispatch d'Auto-Align et vérifié dans le callback avant d'appliquer le résultat.
5. **Échelle px/mm pas toujours rafraîchie** — `updateDynamicScale()` peut être un no-op selon `Config::scaleMethod()`. → calcul géométrique direct depuis la nouvelle homographie (même fallback que le handler 4-points manuel).

### Non corrigé (limitation acceptée)
`validateSize()` reste un no-op quand `expectedPixelsPerMm <= 0` (app jamais calibrée) — le fallback contour perd son principal garde-fou anti-faux-positif dans ce cas. Le seuil de score (point 2 ci-dessus) reste un second filet de sécurité. Documenté dans `docs/AUTO_ALIGN_PLAN.md` ("Risks") plutôt que corrigé ici.

### Leçon
Un `shared_ptr` capturé par valeur dans une lambda de thread d'arrière-plan garde l'objet en vie (pas de UAF), mais ne garantit pas la cohérence **logique** si le code de complétion mélange ensuite des lectures sur "la copie capturée" et sur "le membre live" qui a pu changer pendant l'attente — toujours lire exclusivement depuis la copie capturée une fois qu'on a basculé sur ce pattern.

## ERREUR 37 — Variable locale `tr` masque `QObject::tr()` dans `autoAlignBoard()`

**Date :** 2026-06-18
**Composant :** `src/app/Application.cpp` (`autoAlignBoard()`)
**Statut :** ✅ RÉSOLU

### Symptôme
Premier build réel sur Jetson après le commit de l'audit Auto-Align (#36) :
```
error: no match for call to ‘(const cv::Point_<float>) (const char [38])’
  tr("Auto-Align: aligned via %1 (score %2)")
```
Pas détecté ici (pas de toolchain Qt6/OpenCV dans ce conteneur) — remonté seulement au premier build sur la machine Jetson.

### Cause
Le fix #5 de l'audit #36 (calcul direct du scale px/mm depuis la nouvelle homographie) introduisait deux variables locales `tl`/`tr` (`cv::Point2f`) dans le lambda `finished` de `autoAlignBoard()`. `tr` masque la fonction membre `QObject::tr()` pour le **reste de la portée du lambda** — l'appel `tr("Auto-Align: aligned via %1...")` quelques lignes plus bas tente alors d'appeler l'opérateur `()` sur le `cv::Point2f` local, qui n'existe pas.

Le code existant ailleurs dans le fichier (ex: handler 4-points manuel, ligne ~2058) utilise le même nom `tr` mais sans casser, car la variable y est déclarée dans un sous-bloc `if/else` qui se termine **avant** le prochain appel à `tr(...)`, donc pas de masquage au moment de l'appel — piège purement dépendant de la portée, pas du nom en lui-même.

### Solution appliquée ✅
Renommé les deux variables en `cornerTL`/`cornerTR` dans `autoAlignBoard()`.

### Leçon
Ne jamais nommer une variable locale `tr` (ou tout identifiant Qt courant comme `tr`/`qDebug`/etc.) dans une méthode `QObject`, même si elle semble hors de portée du prochain appel `tr(...)` — un refactor ultérieur peut facilement élargir la portée sans qu'on s'en rende compte. Préférer un nom descriptif (`cornerTL`, `topRight`, …) systématiquement.

## ERREUR 48 — Clic PCB Map ne sélectionne pas le bon composant (nearest-center peu fiable)

**Date** : 2026-06-19
**Composant** : `Application.cpp` — handler `BoardMinimap::anchorRequested`
**Statut** : ✅ RÉSOLU

### Symptôme
« On ne peut toujours pas cliquer sur le composant dans la map pour le sélectionner » : cliquer sur une part dans la PCB Map ne sélectionne pas (ou sélectionne un mauvais) composant, en multi-align comme en sélection simple.

### Cause
Les deux chemins de clic minimap (branche multi-align + branche RealSense) cherchaient le composant dont le **centre** (`c.position`) était le plus proche du point cliqué (`std::hypot` min). Sur une carte dense, le centre d'un gros composant voisin peut être plus proche du clic que le petit composant qu'on vise pile dessus → mauvaise sélection, impression que « ça ne marche pas ».

### Solution appliquée
Nouveau helper `Application::componentAtPcb(cv::Point2f)` : **hit-test bbox** d'abord — parmi les composants Front dont la bbox **contient** le point cliqué, prendre le **plus petit** (le plus spécifique) ; repli sur le centre le plus proche uniquement si le clic tombe sur du board nu (aucune bbox ne contient le point). Utilisé dans les deux chemins. Fichiers `src/app/Application.{h,cpp}`.

### Leçon
Pour « cliquer sur un objet pour le sélectionner », tester l'**appartenance géométrique** (point-dans-bbox/polygone) avant de retomber sur une distance au centre. Le nearest-center seul échoue dès que les objets ont des tailles très différentes et se touchent.

---

## ERREUR 47 — Overlay vibre en Live Tracking sur scène statique (pas de lissage)

**Date** : 2026-06-19
**Composant** : `TrackingWorker.cpp` — Live Tracking (ORB+RANSAC)
**Statut** : ✅ RÉSOLU

### Symptôme
Retour utilisateur (après confirmation que le live tracking fonctionne bien : 4/4 inliers, erreur 0.000px) : « c'est pas mal le live tracking mais ça vibre après si je ne bouge rien ». L'overlay tremble visiblement de quelques pixels alors que la caméra et la carte sont parfaitement immobiles.

### Cause
`processReference()` et `processIncremental()` recalculent une homographie **entièrement nouvelle** à chaque frame traitée (ORB + BFMatcher + `cv::findHomography` RANSAC), sans aucun lissage temporel ni comparaison avec l'estimée précédente. Même sur une scène strictement statique, le bruit de localisation sub-pixel des keypoints ORB (et la sélection aléatoire des inliers par RANSAC) fait varier légèrement le fit d'une frame à l'autre. Cette variation, bien que faible en pixels, est directement visible sur l'overlay qui suit cette homographie à chaque update.

### Solution appliquée
Nouvelle méthode `TrackingWorker::smoothHomography(rawH)` appliquée uniquement à la **valeur émise** (`homographyUpdated`), pas aux accumulateurs internes (`m_cumulativeH`, `m_lastHomography`) :
1. Projette `m_pcbPolygon` (4 coins du board bbox, déjà disponibles via `setBoardPolygon()`) à travers la dernière estimée **lissée** (`m_smoothedHomography`) et la nouvelle estimée **brute**.
2. Mesure le déplacement max des coins projetés entre les deux.
3. Déplacement ≤ 1.5px → bruit, blend très amorti (poids 0.15 sur la nouvelle estimée) ; ≥ 12px → vrai mouvement, blend = 1.0 (aucun lag) ; entre les deux → rampe linéaire.
4. Refit une homographie via `cv::findHomography(pcbPolygon, blendedPts, method=0)` (DLT least-squares).

Repli sans lissage si `m_pcbPolygon` a moins de 4 points. Reset dans `resetReference()`. Fichiers `src/overlay/TrackingWorker.{h,cpp}`.

### Leçon
Une estimée géométrique (homographie, pose...) recalculée indépendamment à chaque frame depuis des données bruitées (keypoints, RANSAC) **vibrera visuellement** même sans mouvement réel, sauf lissage temporel explicite. Le lissage doit comparer un déplacement géométrique concret (ici : déplacement des coins projetés en pixels) plutôt que les coefficients bruts de la matrice — moyenner des matrices d'homographie élément par élément n'a pas de sens géométrique direct.

### Suivi (suite 86) — Phase 1 du plan Live Tracking
Au-delà du lissage (suite 82), 4 mesures s'attaquent aux **causes** (cf. [LIVE_TRACKING_PLAN.md](LIVE_TRACKING_PLAN.md)) : (1) **gate de scène statique** — si l'estimée ne bouge quasi pas vs la dernière émise, ne rien émettre (overlay figé, plus de scintillement) ; (2) **USAC_MAGSAC** + seed fixe (estimateur déterministe, supprime la variation du tirage RANSAC) ; (3) **cornerSubPix** (réduit le jitter de quantification ORB à la source) ; (4) **gate inliers + hystérésis** (fige au lieu de sauter quand le matching se dégrade). Fichiers `src/overlay/TrackingWorker.{h,cpp}`. Phases 2-3 (1€ Filter, modèle adaptatif, GPU) à venir.

---

## ERREUR 46 — « Reset Alignment ne fait rien » (overlay figé)

**Date** : 2026-06-19
**Composant** : `Application.cpp` — pipeline de rendu de l'overlay caméra
**Statut** : ✅ RÉSOLU

### Symptôme
Cliquer le bouton « Reset Alignment » ne change rien visuellement : l'overlay (pads/silkscreen jaunes) reste affiché exactement au même endroit sur l'image caméra.

### Cause
Dans le handler de frame, le bloc qui dessine l'overlay et appelle `cameraView()->setOverlayImage(overlay)` est gardé par `if (m_ibomProject && m_homography && m_homography->isValid())`. Reset appelle `m_homography->reset()` → `isValid()` devient `false` → le bloc est **entièrement sauté**. Or `CameraView` conserve sa dernière `m_overlay` (image membre) et continue de la peindre à chaque `paintEvent` : comme `setOverlayImage()` n'est plus jamais rappelé, **la dernière image d'overlay reste figée** à l'écran. Reset modifiait bien l'état interne mais l'affichage ne se mettait jamais à jour → impression que « ça ne fait rien ».

### Solution appliquée
Ajout d'un `else if (!m_pickingHomographyPoints)` après le bloc overlay : quand il n'y a pas d'homographie valide, pousser une image transparente `setOverlayImage(QImage())` pour **effacer** l'overlay résiduel. Le cas `m_pickingHomographyPoints` est exclu car le picking 4-points dessine son propre overlay juste après. Fichier `src/app/Application.cpp`.

### Leçon
Un widget qui met en cache une image (`m_overlay`) doit être **explicitement** vidé quand la source disparaît : un `if` qui saute le push laisse la dernière image affichée. Toujours prévoir le chemin « plus rien à afficher », pas seulement « voici la nouvelle image ».

---

## ERREUR 45 — `pin1` iBOM (entier) non détecté

**Date** : 2026-06-19
**Composant** : `IBomParser::parsePads()`
**Statut** : ✅ RÉSOLU

### Symptôme
En multi-align, choisir « Pin 1 » sur U7 (module ESP32) affiche « U7 has no pin-1 pad in the iBOM — use the corners method ». Or l'iBOM **connaît** la pin 1 de U7 (visible/surlignée dans le viewer iBOM, pointée par l'utilisateur). La pin 1 n'est détectée pour **aucune** part dont le pad pin 1 n'est pas nommé exactement "1"/"A1".

### Cause
Le parser ne lisait le champ `pin1` du pad **que s'il était un booléen** (`p["pin1"].is_boolean()`). Or dans le JSON iBOM réel, `pin1` est encodé en **entier** (`1`/`0`). `is_boolean()` renvoyait donc `false` → on tombait sur l'heuristique de repli `pad.pinNumber == "1" || "A1"`. Pour un module ESP32, le pad pin 1 a un `num` qui n'est pas "1" → jamais marqué `isPin1`.

### Solution appliquée
`parsePads()` : si le pad contient `pin1`, l'interpréter quel que soit le type — **booléen** (`get<bool>`), **nombre** (`!= 0`), ou **string** ("1"/"true") ; sinon repli sur le `num`. Fichier `src/ibom/IBomParser.cpp`.

### Leçon
Ne jamais présumer du **type JSON** d'un champ iBOM : `pin1`, comme beaucoup de flags iBOM, est un entier, pas un booléen. Lire défensivement (number OU bool OU string).

---

## ERREUR 44 — Auto-Align depth faible score, contour jamais essayé

**Date** : 2026-06-19
**Composant** : `BoardLocator::locate()`
**Statut** : ✅ RÉSOLU

### Symptôme
Sur D405, carte posée à plat avec une **feuille blanche** glissée dessous (pour aider). Auto-Align "réussit" mais l'overlay déborde nettement sur la gauche (hors carte). Log : `BoardLocator: Board located via depth, edge-agreement score 0,13` puis `Auto-Align succeeded via depth (score 0.13)`. Score 0.13 = juste au-dessus de `kMinAcceptableScore = 0.10` → accepté mais placement médiocre. L'utilisateur : « mais j'ai mis une feuille blanche dessous » — et pourtant ça ne change rien.

### Cause
Deux problèmes combinés :
1. **La feuille blanche n'aide que le contour 2D**, pas la profondeur. `locateViaDepth()` segmente un plan par distance — totalement aveugle à la couleur/luminance. Une feuille blanche **coplanaire** avec la carte (même hauteur) ne fait aucune différence en profondeur : le plan ±15mm englobe toujours carte + feuille.
2. **`locate()` essayait la profondeur en premier et, dès qu'elle réussissait, ne tentait JAMAIS le contour** (`if (depth) … else { contour }`). Donc même quand la profondeur produit un quad médiocre (score 0.13), le contour 2D — qui lui exploiterait le contraste vert-sur-blanc de la feuille — n'était pas exécuté. La feuille blanche était donc structurellement inutilisable.

### Solution appliquée ✅
`locate()` réécrit en **course des deux méthodes quand le score profondeur est faible** :
- Profondeur essayée + désambiguïsée → score réel.
- Si pas de résultat **ou** score < `kStrongScore = 0.30` : on lance **aussi** le contour, on le désambiguïse, et on **garde le meilleur score** des deux (le contour ne remplace que s'il est strictement supérieur et passe `kMinAcceptableScore`).
- Au-dessus de `kStrongScore`, la profondeur est jugée fiable → contour sauté (gain de temps).
- Message d'échec combiné (Depth + Contour) si rien d'exploitable.

Ainsi la feuille blanche paie enfin : sur carte coplanaire, la profondeur fusionne carte+surface (score bas), mais le contour détecte les arêtes nettes carte verte/feuille blanche et gagne.

### Leçon
Quand on offre plusieurs stratégies de détection avec des forces complémentaires (profondeur = robuste au contraste mais aveugle à la couplanarité ; contour = sensible au contraste mais exploite la couleur), ne pas faire un simple fallback « A sinon B » : un succès **faible** de A masque B. Préférer une course « si A est faible, essayer B et garder le meilleur ». Et bien expliquer à l'utilisateur quel levier agit sur quelle méthode (la feuille blanche → contour, la surélévation → profondeur) — sinon il optimise pour la mauvaise.

## ERREUR 43 — Segfault à la sortie de l'application

**Date** : 2026-06-19
**Composant** : `Application` — arrêt/destruction
**Statut** : 🔴 OUVERT

### Symptôme
Dans le log terminal fourni par l'utilisateur, juste après la fermeture normale de la fenêtre :
```
[09:21:11.877] [info] [:] Application exiting with code 0
QMainWindow::saveState(): 'objectName' not set for QToolBar 0xaaaaef79a040 'Main'
Segmentation fault (core dumped)
```
Le message « exiting with code 0 » et le warning Qt sortent **avant** le crash — donc `main()` est sorti proprement, et le segfault survient pendant le déroulement des destructeurs/cleanup process (probablement lié à l'ordre de destruction des `QThread`s : `m_trackingThread`/`m_datasetThread`, ou à un objet Qt détruit après son parent).

### Cause
Non investigué. Pistes à explorer la prochaine fois que ça se reproduit :
- Ordre de destruction `Application::~Application()` vs `QApplication` (créée dans `main.cpp` avant `Application`, donc devrait être détruite après — à vérifier que rien dans `~Application()` ne déclenche un signal/slot Qt après que `QApplication` a commencé sa propre destruction).
- Les deux `QThread` dédiés (`m_trackingThread`, `m_datasetThread`) : `quit()`/`wait()` bien appelés avant la destruction du worker (`deleteLater` sur `finished`) ? Un `deleteLater` qui n'a pas le temps de s'exécuter avant la fin de la boucle d'événements peut laisser un objet Qt à moitié détruit.
- Corrélation avec le warning `QMainWindow::saveState(): 'objectName' not set for QToolBar 'Main'` — possible mais pas confirmé ; ce warning seul ne devrait pas crasher.

### Solution appliquée
Aucune — pas encore reproduit/isolé. Noté ici uniquement parce que l'utilisateur en a fourni le log, par souci de ne pas le perdre.

### Leçon
Si ce crash se reproduit et qu'on a le temps d'investiguer : lancer sous `gdb`/`valgrind --tool=memcheck` au moment du `Stop-Process`/fermeture de fenêtre pour obtenir une stack trace, plutôt que d'essayer de deviner depuis le seul "exiting with code 0" + segfault.

## ERREUR 42 — Clic minimap déplace l'overlay sur RealSense au lieu de highlighter

**Date** : 2026-06-19
**Composant** : `Application.cpp` (handler `BoardMinimap::anchorRequested`) / `src/gui/BoardMinimap.{h,cpp}`
**Statut** : ✅ RÉSOLU

### Symptôme
Utilisateur sur D405 : « je vois que quand on clique sur la mini map ça ne fonctionne toujours pas. ça fais bouger tout l'overlay de la carte alors ça devrait highlighter le composant. » Log terminal confirme : chaque clic minimap produit `Homography computed: 4/4 inliers, error: 0.000 px` + `Minimap anchor: PCB (x, y) → image center` — l'overlay entier se redéplace/réoriente à chaque clic.

### Cause
`BoardMinimap::anchorRequested(pcbPoint)` était câblé sans condition de backend à un re-ancrage 1-point (recalcule l'homographie complète pour centrer le FOV caméra sur le point cliqué). Cette fonctionnalité est pensée pour le **microscope à FOV étroit** (`docs/MICROSCOPE_PLACEMENT_PLAN.md`) où toute la carte n'est jamais visible en même temps — recentrer la vue caméra sur un point cliqué a du sens là. Sur **RealSense (FOV large)**, la carte entière est déjà visible : « recentrer le FOV » n'a aucun sens et produit juste l'effet rapporté (overlay qui sursaute à chaque clic anodin).

### Solution appliquée ✅
Handler bifurqué selon `m_config->cameraBackend()` :
- **RealSense** : recherche linéaire du composant `Layer::Front` le plus proche du point cliqué (distance euclidienne en mm, pas de seuil de distance — toujours le plus proche), puis applique exactement le même effet qu'un clic dans le BOM panel (`m_overlayRenderer->setHighlightedRefs()`, `boardMinimap()->setSelectedRef()`, `bomPanel()->highlightComponent()`). Aucune homographie touchée.
- **Microscope (V4L2)** : comportement de re-ancrage inchangé.

### Leçon
Une fonctionnalité conçue pour un cas d'usage spécifique (FOV étroit) ne doit pas être câblée globalement sur tous les backends sans vérifier qu'elle reste pertinente pour chacun — ici le bug n'était pas un défaut d'implémentation de l'anchor lui-même (l'homographie était calculée correctement, 0.000px d'erreur) mais un mauvais choix de comportement par défaut pour le backend RealSense.

## ERREUR 41 — Auto-Align D405 : carte coplanaire avec la table

**Date :** 2026-06-18
**Composant :** `src/overlay/BoardLocator.cpp` (`locateViaDepth()` + `validateSize()`)
**Statut :** 🟡 CONTOURNÉ (message corrigé + workaround physique ; pas de séparation logicielle carte/fond coplanaire à ce stade)

### Symptôme
Auto-Align D405 **intermittent** sur la même scène (log utilisateur, distance 72mm, scale 6.1 px/mm, depth fill 87%) :
```
21:04:04 BoardLocator: Board located via depth, edge-agreement score 0,26
21:04:04 Auto-Align succeeded via depth (score 0.26)          ← SUCCÈS
21:04:32 candidate quad area (356855 px^2) doesn't match the board outline
         at the known scale (142237 px^2)                     ← ÉCHEC
21:05:03 candidate quad area (355897 px^2) ... (142237 px^2)  ← ÉCHEC
```
Ratio aire = 356855 / 142237 = **2.509×**, juste au-dessus de `kAreaTolerance = 2.5` (≈1.58× en linéaire). La même scène réussit puis échoue sans réalignement.

### Cause
La carte est posée **à plat sur la table en bois** → carte et table autour sont à la **même distance** (coplanaires). `locateViaDepth()` prend la médiane de profondeur sur la ROI centrale (= surface carte, 72mm) puis masque tous les pixels à ±`kDepthBandMm` (15mm) de cette référence — ce qui inclut la carte **plus une marge de table tout autour** au même plan. Le `minAreaRect` du plus grand contour englobe donc carte+table → ~1.58× trop grand en linéaire. Comme on est pile sur le seuil (2.509 vs 2.5), le résultat **dépend de la quantité de table coplanaire visible à l'instant t** (angle/cadrage de la carte) : quand la carte ressort assez (moins de table dans le plan), l'aire matche → succès (21:04:04) ; un instant plus tard, plus de table balayée dans le plan → 2.5× → rejet. C'est une **limite fondamentale** de la segmentation par plan de profondeur quand la carte est à fleur d'une surface : aucune marche de profondeur ne les sépare.

### Pourquoi ne PAS juste élargir `kAreaTolerance`
Accepter le quad de 356855 px² (carte+table) ferait mapper les coins de la **carte** sur les coins de la **table** → overlay trop grand et mal placé (exactement le symptôme « overlay en haut à droite / mal placé » des sessions précédentes). Le garde-fou taille fait son travail ici ; le desserrer réintroduirait le mauvais placement.

### Solution appliquée (partielle) ✅
- `validateSize()` : message **directionnel** au lieu du « make sure the whole board is in frame » trompeur (qui suggère « trop petit / hors cadre » alors qu'ici c'est « trop grand »). Si `ratio > kAreaTolerance` → « detected region … is larger than the board … probably merged with a coplanar background. Lift the board off the table/surface so it stands out in depth, or use manual alignment ». Si `ratio < 1/kAreaTolerance` → message « smaller … make sure the whole board is in frame ».

### Workaround utilisateur (immédiat, fiable)
- **Surélever la carte** de la table (la poser sur une petite boîte/support) → elle ressort en profondeur, le plan ±15mm n'attrape plus la table, l'aire matche → Auto-Align fiable. C'est le mode d'emploi attendu de la méthode depth.
- Ou **alignement manuel 4-points** — confirmé fonctionnel dans le même log (21:06, « Manual homography computed successfully, error=0.000px »).

### Piste de fix logiciel (non implémentée — à valider avec l'utilisateur avant)
Pour gérer le cas « carte à plat sur surface coplanaire » sans intervention physique, il faudrait **raffiner le plan de profondeur avec un signal couleur/arête** : à l'intérieur du masque de plan, isoler la sous-région à forte densité d'arêtes (PCB = traces/silkscreen/composants, très texturé) du fond relativement uniforme (table), puis fitter le `minAreaRect` sur cette sous-région seulement. Risqué à coder à l'aveugle (pas de matériel ici) — peut régresser le cas qui marche (carte surélevée). Reporté tant que l'utilisateur ne le demande pas explicitement.

### Leçon
La segmentation par plan de profondeur suppose que l'objet est le plan le plus proche **distinct** — un objet à fleur d'une surface plus grande viole cette hypothèse et la profondeur seule ne peut pas les séparer. Quand un garde-fou taille tape pile sur le seuil (2.51 vs 2.5), le comportement devient intermittent et dépendant du cadrage : signe qu'il faut un signal supplémentaire (couleur/arête) ou une contrainte de scène (surélever), pas un simple ajustement de seuil.

---

## ERREUR 40 — `setTotalComponents()` jamais appelé — Inspection Progress toujours à zéro

**Date :** 2026-06-18
**Composant :** `src/gui/StatsPanel.{h,cpp}`
**Statut :** 🔴 OUVERT (constaté en creusant un rapport utilisateur, pas encore corrigé)

### Symptôme
Sur un screenshot utilisateur (D405, carte visible, après reboot de l'app), le panneau « Inspection Progress » affiche « No inspection data » et 0% alors que la carte semble correctement chargée (overlay partiellement visible). `Placed`/`Missing`/`Defects`/`Pending` sont tous à 0.

### Cause
`StatsPanel::updateSummaryLabel()` affiche « No inspection data » dès que `m_total == 0` (`StatsPanel.cpp:336`). `m_total` n'est modifié que par `StatsPanel::setTotalComponents(int total)` (`StatsPanel.cpp:214`) — **grep sur tout `src/` : cette méthode n'est appelée nulle part**, y compris dans `Application.cpp` au chargement d'un iBOM (`loadIBomFile()`) ou au démarrage d'inspection (`startInspection()`). Le panneau affichera donc **toujours** « No inspection data » / 0%, qu'un iBOM soit chargé ou non — l'indicateur ne reflète rien.

### Impact sur le diagnostic en cours
Repéré en tentant de déterminer, à partir d'un screenshot, si un iBOM était chargé au moment où l'utilisateur a signalé qu'« Auto-Align ne fonctionne pas » après reboot. Ce panneau ne peut **pas** servir de signal pour ça tant qu'il n'est pas câblé — toujours à 0 par construction actuelle, qu'on ait un projet chargé ou pas.

### Solution (pas encore appliquée)
Appeler `m_mainWindow->statsPanel()->setTotalComponents(...)` après chargement d'un iBOM réussi (`loadIBomFile()`, count = `m_ibomProject->components.size()`) et le maintenir à jour quand `m_placedRefs` change (placement/retrait de composants). Périmètre exact (quels événements doivent incrémenter `Placed`/`Missing`/`Defects`) à clarifier avant d'implémenter — pas fait dans cette session pour rester focalisé sur le rapport glare/Auto-Align en cours.

### Leçon
Une méthode publique définie mais jamais appelée dans un GUI Qt ne provoque ni erreur de compilation ni warning évident — seul un grep cross-fichier (`setTotalComponents(` dans tout `src/`) l'a révélée. À surveiller : tout nouveau setter ajouté à un panneau d'affichage doit être immédiatement câblé à un site d'appel, sinon il devient un panneau mort silencieux.

---

## ERREUR 39 — Distance/Auto-Align/self-cal faux sous glare D405

**Date :** 2026-06-18
**Composant :** `src/overlay/BoardLocator.cpp` (`locateViaDepth()`), `src/app/Application.cpp` (handler `depthFrameReady`), `src/camera/RealSenseCapture.cpp` (self-cal on-chip), `src/gui/StatsPanel.cpp` (Event Log)
**Statut :** ✅ RÉSOLU (code) — ⚠️ non testé sur le matériel réel à ce stade

### Symptôme
Deux rapports utilisateur distincts, même session :
1. Screenshot D405 avec un reflet/éblouissement visible sur la carte (carte glossy) : panneau Statistics affiche **« Distance: 104.0 mm »** alors que la distance réelle mesurée par l'utilisateur est **~70 mm**, et **« Depth fill: 11% »**. Auto-Align échoue : `BoardLocator::locateViaDepth()` trouve un candidat de **4702 px²** contre une aire de carte attendue de **123295 px²** (~26× trop petit).
2. Après reboot de l'app : self-calibration on-chip D405 échoue ses 3 tentatives avec le message **« Not enough ... »** (tronqué dans la colonne Message de l'Event Log), puis « RealSense streaming pipeline restored after calibration » (retour au pipeline normal, calibration usine conservée).

### Cause
Reflet spéculaire / éblouissement sur une carte PCB glossy confond l'appariement stéréo IR du D405 → une grande partie de la frame de profondeur devient invalide (pixels à 0). Conséquences en cascade, toutes dérivées de cette même frame corrompue :
- La médiane sur la ROI centrale (`Application.cpp`, handler `depthFrameReady`) mélange échantillons valides et bords de zones invalides → distance fausse mais plausible (104mm au lieu de 70mm).
- `BoardLocator::locateViaDepth()` segmente le plan le plus proche à partir du masque profondeur → avec 11% de pixels valides, le contour obtenu est minuscule/sans rapport avec la carte réelle (4702 px² vs 123295 attendus) plutôt que de représenter la vraie carte.
- Recherche web (confirmée) : le firmware D4xx lui-même applique un garde-fou identique pour sa propre self-calibration on-chip — message officiel `"Not enough depth pixels! (Fill_Factor_LOW). Please retry in different lighting conditions"` ([GitHub issues IntelRealSense/librealsense](https://github.com/IntelRealSense/librealsense/issues/10822)). C'est exactement le même phénomène (fill factor bas) qui fait échouer la self-cal ET corrompt la distance/Auto-Align — pas une coïncidence, une seule cause physique (glare) avec trois symptômes différents.

### Solution appliquée ✅
1. **`BoardLocator.cpp`** : nouvelle constante `kMinDepthFillRatio = 0.20` (espace de noms anonyme, à côté de `kMinAcceptableScore`). `locateViaDepth()` calcule le ratio de pixels non-nuls sur toute la frame **avant** toute segmentation et échoue explicitement (`reason = "depth data too sparse (X% valid) — likely glare/reflection off the board surface; reduce lighting glare or try the contour method"`) si sous le seuil — au lieu de segmenter un plan minuscule/faux et de laisser `validateSize()` le rejeter (ou pire, l'accepter) sans explication claire.
2. **`Application.cpp`** (handler `depthFrameReady`) : même garde, calculée une fois et réutilisée pour `sp->setFillRate(...)` puis comparée à `kMinDepthFillRatio` (dupliqué localement, même valeur 0.20) — sous le seuil, `sp->setDistance(0.0)` (affiche « — », sentinelle déjà gérée) et **return** avant le calcul de la médiane/`m_lastDepthDistanceMm`/scale px/mm, qui ne seraient plus mis à jour avec une valeur dérivée d'une frame majoritairement invalide.
3. **`RealSenseCapture.cpp`** (bloc self-cal on-chip) : nouveau `else if (lastErr.find("Not enough") != std::string::npos)` dans la construction du message d'aide après les 3 tentatives échouées — distinct du cas déjà géré `"HW not ready"` ([ERREUR #30](#erreur-30--self-calibration-d405-hw-not-ready)).
   - **Correction importante (suite 64)** : l'utilisateur a montré une capture où **Depth fill = 86%** (live stream sain) mais la self-cal échoue quand même « Not enough … ». **Le fill du live stream est sans rapport avec le succès de l'OCC** : la self-cal tourne sur **son propre profil 256×144@90** (cf. [ERREUR #30](#erreur-30--self-calibration-d405-hw-not-ready), contrainte firmware) et échantillonne une **région centrale**. La vraie exigence Intel ([doc self-calibration D400](https://dev.realsenseai.com/docs/intel-realsense-self-calibration-for-d400-series-depth-cameras/), [page produit](https://www.intelrealsense.com/self-calibration-for-depth-cameras/)) : **surface plane, texturée, remplissant tout le champ de vision**, à distance modérée, caméra perpendiculaire. Une petite carte PCB inclinée à ~7cm (limite proche du D405) ne satisfait pas ça → `Fill_Factor_LOW`. Message corrigé en conséquence : demande une surface plane/texturée remplissant le cadre, précise que c'est **indépendant du « Depth fill % » live**, et rappelle que l'OCC est **optionnelle** (réduit seulement le bruit de profondeur ; la calibration usine reste valide et Auto-Align/overlay marchent sans).
4. **`StatsPanel.cpp`** (`appendEventRow()`) : `msgItem->setToolTip(message)` — la colonne Message de l'Event Log tronque visuellement les messages longs (c'est ce « Not enough … » tronqué qui a motivé ce fix) ; le texte complet était auparavant seulement récupérable via le fichier log, pas dans l'UI.

### Leçon
Un taux de remplissage (« fill ratio ») bas sur une frame de profondeur doit être traité comme **donnée invalide à rejeter explicitement**, pas comme une mesure bruitée à exploiter quand même — toute grandeur dérivée (distance médiane, segmentation de plan, scale px/mm sténopé) devient alors fausse-mais-précise-en-apparence, ce qui est strictement pire qu'un échec clair (un nombre qui a l'air correct n'invite pas à la méfiance). Le seuil choisi (20%) n'est pas arbitraire : le firmware D4xx applique le même principe en interne pour sa propre self-calibration, ce qui confirme la pertinence de la garde côté application plutôt que de compter uniquement sur le firmware pour s'en apercevoir trop tard (et seulement pour la self-cal, pas pour Auto-Align ni l'affichage Distance).

**À valider au prochain build Jetson** : sous glare/reflet (Depth fill bas), la Distance doit afficher « — » plutôt qu'une valeur fausse, Auto-Align doit échouer proprement avec le message glare au lieu de proposer un mauvais placement, et le message complet d'un échec self-cal doit être lisible au survol dans l'Event Log.

**Note (suite 64)** : la self-cal qui « ne passe pas » à 86% de fill n'est **pas** un bug applicatif — c'est un échec attendu de l'OCC firmware quand la scène ne remplit pas ses exigences (surface plane texturée plein cadre). Côté code on ne peut que mieux l'expliquer (fait) ; la calibration usine suffit pour l'usage de l'app. Aucune action code supplémentaire prévue tant que l'utilisateur ne demande pas spécifiquement à faire passer l'OCC (auquel cas → pointer une surface texturée plein cadre, ou retirer carrément le bouton self-cal s'il prête à confusion).

---

## ERREUR 38 — Auto-Align échoue sur D405 : scale px/mm périmé

**Date :** 2026-06-18
**Composant :** `src/app/Application.cpp` (`autoAlignBoard()`), `src/overlay/BoardLocator.cpp` (`validateSize()`)
**Statut :** ✅ RÉSOLU

### Symptôme
Premier test réel d'Auto-Align sur D405 (log utilisateur) :
```
BoardLocator: Depth: candidate quad area (381762 px^2) doesn't match the board
outline at the known scale (32771 px^2) — make sure the whole board is in frame.
Contour: no board-shaped quad found in the frame...
Auto-Align failed: ...
```
Ratio aire ≈ 11.65× (≈ 3.4× en linéaire) — bien au-delà de `kAreaTolerance = 2.5`. La méthode profondeur a très probablement trouvé le **vrai** plan de la carte (c'est tout l'intérêt de la segmentation par profondeur), mais `validateSize()` l'a rejeté car le `expectedPixelsPerMm` passé à `BoardLocator::locate()` était faux.

### Cause
`autoAlignBoard()` calculait `expectedPixelsPerMm` depuis `m_currentPixelsPerMm`/`m_basePixelsPerMm`, eux-mêmes alimentés soit par la dernière calibration checkerboard sauvegardée (`m_calibration->pixelsPerMm()`, potentiellement faite avec une autre caméra ou à une autre distance de travail), soit par un live-update **uniquement si `Config::scaleMethod() == ScaleMethod::Depth`** (cf. handler `depthFrameReady`, ligne ~1382). Avec `scaleMethod` sur autre chose que `Depth` (valeur par défaut probable), aucun de ces deux chemins ne reflète l'échelle réelle de la D405 à sa distance de travail actuelle → écart de 3.4× exactement le genre d'erreur que `validateSize()` est censé éviter de laisser passer à l'envers (ici elle bloque un **vrai positif**).

### Solution appliquée ✅
- Nouveau membre `Application::m_lastDepthDistanceMm`, mis à jour **sans condition** sur `scaleMethod()` dans le handler `depthFrameReady` (juste après `sp->setDistance(distMm)`) — donc toujours disponible dès qu'un flux profondeur D405 arrive, indépendamment du réglage d'échelle choisi par l'utilisateur.
- `autoAlignBoard()` calcule maintenant `expectedPixelsPerMm` en priorité via la géométrie sténopé (`fx / distance_mm`, `fx` via `RealSenseCapture::colorFx()`) quand une D405 + une distance valide sont disponibles — c'est exactement la même formule déjà utilisée par le live-update `ScaleMethod::Depth`, juste rendue disponible à Auto-Align sans dépendre de ce réglage. Fallback inchangé (`m_currentPixelsPerMm` puis `m_basePixelsPerMm`) si pas de D405/distance.

### Leçon
Une calibration checkerboard sauvegardée encode une distance de travail implicite — la réutiliser pour valider la taille d'un contour détecté à une distance différente (autre caméra, autre setup) peut rejeter un résultat **correct**. Quand une mesure de distance physique directe est disponible (profondeur D405), préférer la dériver à la volée plutôt que de faire confiance à un scale mis en cache, même récent.
