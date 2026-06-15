# Journal de Session — Migration Jetson AGX Orin

> **But du document** : suivre chronologiquement la progression de la migration vers Jetson, et permettre de reprendre rapidement à toute session.
>
> **Convention** : sessions en ordre **antichronologique** (la plus récente en haut). Le bloc "État actuel" tout en haut est mis à jour à chaque session.
>
> **Documents liés** :
> - [JETSON_MIGRATION.md](JETSON_MIGRATION.md) — plan global + architecture
> - [JETSON_ERREURS.md](JETSON_ERREURS.md) — journal des bugs rencontrés
> - [docker/README.md](../docker/README.md) — quickstart Docker

---

## État actuel — au 2026-06-15 (RealSense Phase 1 codée, à valider Jetson)

> **2026-06-15 (suite 10)** : **profils de résolution** dans le panneau RealSense. 4 profils 1-clic (Précision depth 848×480 recommandé / Détail visuel 1280×720 / Remplissage maximal 848×480+holeFill / Aperçu rapide 480×270@60), chacun avec **description gains/pertes**. Appliquer = résout le Visual Preset depuis les labels SDK → `setPendingVisualPreset` (appliqué par le thread capture au (re)start, point fiable), règle les filtres via `setControl`, puis stop/setResolution/setFps/start + rebuild différé. `RealSenseCapture` : `setPendingVisualPreset()` + `m_pendingPreset` appliqué dans `captureLoop` après publication du device. UI dans `RealSenseControlsDialog` (combo + label description + bouton Appliquer).
>
> **2026-06-15 (suite 9)** : **filtres depth configurables dans le panneau + réglages D405 recommandés**. La chaîne de filtres (Spatial/Temporal/Threshold/Hole Filling) passe en membre pimpl `FilterChain` de `RealSenseCapture` (mutex, on/off par filtre, défaut spatial+temporal). `listControls()`/`setControl()` énumèrent et pilotent désormais **aussi** les options des filtres (ownerId ≥ `kFilterBase`, toggle synthétique « Enabled » à `kEnableOption`) — helper `enumerateOptions(rs2::options&)` partagé sensors/filtres. Le panneau Phase 2.5 affiche donc les groupes filtres avec case Enabled + tous leurs paramètres (tooltips SDK). Défaut résolution RealSense passé à **848×480** (précision depth optimale) — non écrasé par le 1920×1080 du Config (V4L2). Doc : tableau réglages recommandés D405 (preset High Accuracy, expo manuelle + gain 16, polariseur linéaire pour reflets, disparity shift seulement < 7 cm) dans REALSENSE_D405_PLAN.md §Phase 2.5.
>
> **2026-06-15 (suite 8)** : **post-processing depth** (issue #10682, PDF fourni). La D405 (pas d'IR/projecteur) bruite sur surfaces peu texturées → `RealSenseCapture` applique désormais **Spatial + Temporal filter** (rs2, défauts Intel alpha 0.4 / delta 20 / persistency 3, identiques au snippet de l'issue) sur la depth alignée avant émission. Améliore distance/scale (Phase 2) et surtout l'inspection 3D (Phase 3). Filtres préservant la résolution → alignement couleur↔depth intact. **Note Phase 3** : prévoir un bon éclairage / motif visible sur les zones lisses (plan de masse, solder mask) — la depth y est intrinsèquement bruitée sur D405.
>
> **2026-06-15 (suite 7)** : **Phase 2.5 — panneau d'options capteur RealSense** (comme le Viewer Windows). `RealSenseCapture` publie le `rs2::device` live (mutex) + `listControls()` (descripteurs `RsControl` : range, valeur, read-only, **`get_option_description()` → tooltip**) + `setControl()`. Nouveau `gui/RealSenseControlsDialog` qui **génère dynamiquement** un contrôle par option supportée (checkbox/spinbox), groupé par sensor, application immédiate, bouton Refresh. Bouton « Camera Controls (RealSense)… » dans `ControlPanel` → `Application` ouvre le dialog (messages si backend ≠ RealSense / caméra arrêtée). Compilé seulement si `IBOM_HAVE_REALSENSE`. **Maj** : les options à valeurs discrètes (Visual Preset, Power Line Frequency…) rendues en **combo box** avec les noms du SDK (`get_option_value_description`) au lieu d'un spinbox. **Note D405** (issue librealsense #10682) : la D405 n'a **pas** de capteur RGB séparé (couleur produite par le module stéréo) ni d'emitter laser → notre énumération dynamique gère ça nativement (un seul groupe sensor, options non supportées masquées). ⚠️ À valider D405 branchée.
>
> **2026-06-15 (suite 6)** : **plan détaillé Phase 3** (inspection 3D) → nouveau doc **[REALSENSE_D405_PHASE3_PLAN.md](REALSENSE_D405_PHASE3_PLAN.md)**. Idée centrale : plan de référence PCB (médian d'abord, RANSAC ensuite) → height map → 4 features (heatmap hauteur, validation hauteur par composant iBOM, tombstoning, soulevé/pont) + export PLY. Architecture `DepthInspector` (QThread dédié) + `HeightMapOverlay`. Découpage 3a→3d. Pas codé — attend validation Phases 1-2 sur D405 réelle.
>
> **2026-06-15 (suite 5)** : **RealSense D405 — Phase 2 (profondeur)**. ✅ Phase 1 **compile et link** sur Jetson (`[85/86] Linking … bin/MicroscopeIBOM`, `RealSense: ENABLED (2.55.1)`, que des warnings pré-existants). Phase 2 codée : `RealSenseCapture` active le flux **depth Z16 + `rs2::align(color)`**, émet `depthFrameReady(DepthFrameRef)` (CV_16UC1 en mm, aligné couleur) et cache `fx` des intrinsèques usine (`colorFx()`). `DepthFrameRef` (= `FrameRef`) enregistré sous son nom alias pour le QueuedConnection. `StatsPanel::setDistance()` → ligne « Distance: NN mm ». `Application::wireCameraSignals` connecte la depth via `dynamic_cast<RealSenseCapture*>` (throttle 3 Hz, médiane ROI centrale 20% en ignorant les 0) → distance live + **scale px/mm auto = fx / distance_mm** quand `ScaleMethod::Depth` choisi. `Config` : `ScaleMethod::Depth=3` + entrée combo Settings « From RealSense depth ». ⚠️ À valider avec la D405 branchée.
>
> **2026-06-15 (suite 4)** : **RealSense D405 — Phase 1 implémentée (couleur dual-backend)**. Nouvelle interface `src/camera/ICameraSource.h` (QObject abstrait, foyer de `FrameRef` + signaux `frameReady/captureError/captureStateChanged`). `CameraCapture` en hérite (microscope V4L2 inchangé). Nouvelle classe `RealSenseCapture.{h,cpp}` (librealsense2, flux couleur BGR8, thread dédié, `try_wait_for_frames` borné, fallback mode par défaut si 1920×1080 non supporté par la D405, `listDevices()` via `rs2::context`). `Config` : `CameraBackend{V4L2,RealSense}` + clé JSON `camera.backend` (défaut v4l2, migration auto). `Application` : `m_camera` → `unique_ptr<ICameraSource>` ; helpers `createCamera()` / `wireCameraSignals()` / `switchCameraBackend()` / `refreshCameraDeviceList()` → **hot-swap de backend** (le gros lambda `frameReady` extrait dans `wireCameraSignals`, le reste de `connectSignals` dans `connectControlSignals`). `SettingsDialog` : combo « Backend » (Microscope USB / RealSense D405), énumération backend-aware. CMake : option `IBOM_ENABLE_REALSENSE`, `find_package(realsense2)`, source compilée + define `IBOM_HAVE_REALSENSE` **uniquement si trouvée** (sinon UI masque le backend, zéro régression). Scripts `run_local_gui.sh`/`run_dev_shell.sh` : détection D405 (`lsusb 8086:0b*`) → mapping `/dev/bus/usb` (RSUSB libusb, pas `/dev/video*`). ⚠️ À compiler/valider sur Jetson (`bash scripts/build_jetson.sh`, vérifier `RealSense: ENABLED` dans le résumé CMake + `rs-enumerate-devices`).
>
> **2026-06-15 (suite 3)** : **plan d'intégration caméra Intel RealSense D405** → nouveau document **[REALSENSE_D405_PLAN.md](REALSENSE_D405_PLAN.md)**. Architecture **dual-backend** (interface `ICameraSource` → `CameraCapture` USB **et** `RealSenseCapture` D405, sélection runtime, microscope jamais retiré). Constat clé : `librealsense2 v2.55.1` est **déjà compilée avec CUDA** dans `base.Dockerfile` (STAGE 2) + udev rules → reste à faire le `find_package(realsense2)` CMake + la couche C++. Phase 1 = couleur dual-backend, Phase 2 = profondeur (scale auto + distance), Phase 3 = inspection 3D. **Section calibration ajoutée** (§11) : intrinsèques RGB lues en usine via librealsense (pas de damier), profondeur via **On-Chip Calibration sans cible** (les cibles Intel UCAL / barres 10 mm sont inutilisables à la distance de travail courte de la D405 — FOV trop étroit). Branche `claude/realsense-d405` (depuis `main`). PR #5 fermée (redondante), #6 mergée dans `main`.
>
> **2026-06-15 (suite 2)** : **fix ascenseur dock gauche (souris)** — le `QScroller::LeftMouseButtonGesture` ajouté pour le tactile capturait le bouton gauche et cassait la molette + le glissement de l'ascenseur à la souris. Remplacé par `QScroller::TouchGesture` (vrai tactile uniquement, souris native) + policy scrollbar explicite sur `InspectionPanel`. **Cause racine** : `setEnabled(false)` sur le panneau entier désactivait aussi la `QScrollArea` enfant tant qu'aucun iBOM n'était chargé → ne désactiver que le contenu interne (`m_content`).
>
> **2026-06-15 (suite)** : **décodage MJPG hardware NVIDIA** (`CameraCapture`) — pipeline GStreamer `v4l2src ! image/jpeg ! nvv4l2decoder mjpeg=1 ! nvvidconv ! BGR ! appsink`, décodage sur NVDEC/VIC au lieu du CPU. Fallback automatique vers V4L2 si GStreamer absent ou pipeline KO. Toggle `camera.hw_decode` (Config, défaut ON) + case à cocher Settings → Camera. **H.264 écarté volontairement** : compression inter-frames → artefacts de bloc nuisibles au tracking ORB / mesure (cf échange utilisateur). ⚠️ À valider sur Jetson (vérifier `v4l2-ctl --list-formats-ext` que la caméra sort bien du MJPG, et que OpenCV a le backend GStreamer).
>
> **2026-06-15** : fix **mise en page InspectionPanel** (QScrollArea — boutons Export Report plus tronqués/chevauchants) + fix **caméra dans Settings** (SettingsDialog utilisait QMediaDevices au lieu de CameraCapture::listDevices, même bug que #17). Branche `claude/pr5-plus-fixes`. ⚠️ À valider sur Jetson.
>
> **2026-06-12 (suite 3)** : **restauration de session d'inspection** (`session_state.json` clé = chemin iBOM, sauvegarde à chaque « Placed », reprise au chargement + au Start Inspection, Reset efface) + **boutons Report HTML/PDF** (InspectionPanel + menu File → Export Report, via `ReportGenerator` enfin instancié — stats/yield/checklist + snapshot caméra). Nouveau : `PickAndPlace::restorePlaced()`. ⚠️ Toujours rien compilé ici — valider sur Jetson.
>
> **2026-06-12 (suite 2)** : **items 4-6 du backlog implémentés** — focus assist (netteté Laplacien live dans StatsPanel), fichiers récents + auto-reload iBOM (File → Open Recent), **RemoteView câblé** (viewer HTML écrit dans `$IBOM_DATA_DIR/remote_view.html`) + nouvel onglet **Settings → Features**. ⚠️ Toujours rien compilé ici — valider sur Jetson.
>
> **2026-06-12 (suite)** : **quick wins 1.1–1.3 d'IDEES_AMELIORATIONS.md implémentés** (garde-fou anti zip-bomb décompression LZString + test, statut IA dans la status bar, slider confiance câblé Config+détecteur, bonus : `Config::save()` à l'arrêt). ⚠️ **Non compilé** (pas de toolchain ici) — valider sur Jetson : `bash scripts/build_jetson.sh` + `ctest`. Voir l'entrée de session ci-dessous.
>
> **2026-06-12** : analyse complète du code à la demande de l'utilisateur (« trouve des améliorations et des nouvelles features ») → nouveau document **[IDEES_AMELIORATIONS.md](IDEES_AMELIORATIONS.md)** : 3 quick wins (garde-fou décompression iBOM, slider confiance non câblé, statut IA invisible), 7 features dormantes à câbler (RemoteView, ReportGenerator, BarcodeScanner…), 7 nouvelles features (focus assist, fichiers récents, restauration session, InferenceWorker async…), priorisation en §5. Branche `claude/focused-fermi-if21mm`.
>
> **2026-06-11** : PR #2 mergée dans `main` (`f36895e`) — Dataset Studio Lots 1+2 + Phase 1c + pipeline IA + tous les fixes Docker/scripts. Fix `INSTALL.bat` (PR #3), scripts Linux du Studio (PR #4). **Phase A implémentée** : `DatasetCreator` + `DatasetPanel` + signal qualité tracking + `footprint_classes.json` + tests (voir session "suite" ci-dessous). Le dev continue sur `claude/dreamy-cori-oec93c`.
>
> ✅ **Build + ctest Jetson validés** (2026-06-11) : `bash scripts/build_jetson.sh` → 31/31, binaire 1.3 MB. `ctest` → **7/7 passent**. Branche `claude/dreamy-cori-oec93c` mergée dans `main`.

> **Nouveau aujourd'hui** (3 itérations) :
> 1. App **lancée sur l'écran local du Jetson** ✅ + audit complet → [JETSON_AMELIORATIONS.md](JETSON_AMELIORATIONS.md)
> 2. Implémentation des recommandations (persistance `IBOM_DATA_DIR`, FOURCC MJPG, fixes Docker/scripts, test TrackingWorker, CI shell/compose)
> 3. **Phase 1c terminée** (plus aucun code Windows dans le tronc), **pipeline IA câblé** (init auto en arrière-plan si `.onnx` présent — [AI_PIPELINE.md](AI_PIPELINE.md) + `scripts/export_yolov8_onnx.py`), **image runtime minimale** réécrite (multi-stage, jamais buildée).
>
> ⚠️ **À FAIRE en priorité sur le Jetson** : `git checkout docker/compose.local.yml && git pull`, puis `bash scripts/build_jetson.sh` + `cd build && ctest` pour **valider la compilation** — rien de tout ça n'a pu être compilé dans l'environnement d'analyse (pas de CUDA/Qt/OpenCV). Décision utilisateur : **ONNX ne sera jamais optionnel** (pas de build no-GPU).

### Phase courante
**Phase 0 — Conteneurisation** : ✅ **COMPLÈTE** — images `microscope-ibom:base` (avec ONNX Runtime ARM64 compilé from source, CUDA 12.6 + TensorRT 10.3 EP) et `:dev` (avec outils dev) opérationnelles sur Jetson AGX Orin 32GB JP6.2.
**Phase 1a — Portage Linux pur** : ✅ code livré.
**Phase 1b — Build C++ Linux** : ✅ **VALIDÉE** — binaire `build/bin/MicroscopeIBOM` (1.1 MB) généré, link contre Qt6.2 + OpenCV 4.10 CUDA + ONNX Runtime + CUDA cudart, démarrage runtime OK (`GPU: Orin (30698 MB, CUDA 8.7), CUDA 12.6, TensorRT 10.3.0`).
**Phase 2a/b/c — Mémoire unifiée** : ✅ **VALIDÉE RUNTIME** — `test_unified_allocator` ctest pass 6/6 cas, define `IBOM_USE_UMA_ALLOCATOR` actif dans les flags du binaire, probe `cudaMallocManaged` OK.
**Tooling** : ✅ journaux + hook PreCompact + bootstrap one-liner configurés.

### Branches & tags
| Ref | Pointe sur | Statut |
|-----|------------|--------|
| `main` | `f73497f` (Phase 2a/b/c UnifiedAllocator) | actif, dev Jetson |
| `windows-legacy` | `3174dad` (last commit Windows) | gelée, pour repli |
| `v0.1.0-windows-final` (tag) | `3174dad` | archive permanente |

### Matériel
- **Jetson AGX Orin 32GB** (Seeed reComputer J4012) — reçu et opérationnel
- **JetPack 6.2** (L4T R36.4, Ubuntu 22.04) — installé d'origine Seeed
- **Écran tactile Minix SF16T** — prévu (HID-multitouch USB-C standard)
- **Caméra microscope USB** — UVC standard (V4L2)
- **RealSense D405** — prévue (futur, librealsense2 déjà packagée)

### Ce qui est livré
- Plan de migration complet : [docs/JETSON_MIGRATION.md](JETSON_MIGRATION.md)
- Stack Docker complet dans [docker/](../docker/)
- Script de build Jetson : [scripts/build_jetson.sh](../scripts/build_jetson.sh)
- Configuration cross-platform .gitattributes + .dockerignore
- Journaux de session + erreurs : [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md), [JETSON_ERREURS.md](JETSON_ERREURS.md)
- Règles strictes de tenue de journaux dans [CLAUDE.md](../CLAUDE.md) (mise à jour avant chaque push obligatoire)

### Ce qui reste à faire
- [ ] **Brancher la caméra microscope USB** et décommenter les `devices:` dans `docker/compose.yml` pour le service `runtime` (et `dev` si besoin)
- [ ] **Tester `nsys profile`** sur une frame de capture pour confirmer <1ms en copies host↔device (critère Phase 2)
- [ ] **Brancher l'écran tactile Minix SF16T** + tester le X11 forwarding (`xhost +local:docker`)
- [ ] Phase 1c : suppression du code Windows résiduel (`build_windows.bat`, branches `_WIN32` dans le code)
- [ ] Phase 2d : `InferenceEngine` zero-copy preprocess (à faire quand on aura un `.onnx` réel à valider)
- [ ] Phase 2.5 (gros morceau, optionnel) : V4L2 DMABUF direct (hors `cv::VideoCapture`) si la caméra microscope le supporte
- [ ] Phase 3 (optionnel) : DLA + INT8 pour le ComponentDetector
- [ ] Entraîner / exporter un YOLOv8m ONNX pour le ComponentDetector

### Blocages connus
Aucun. Tous les obstacles Phase 0/1/2 sont résolus et documentés dans [JETSON_ERREURS.md](JETSON_ERREURS.md) (13 entrées résolues).

### Comment reprendre à la prochaine session
1. Lire ce bloc "État actuel" + la dernière entrée de session ci-dessous
2. Vérifier le statut de [JETSON_ERREURS.md](JETSON_ERREURS.md) pour les bugs ouverts
3. Sur le Jetson : `cd ~/Assistant-git && git pull && git status`
4. Continuer là où la dernière session s'est arrêtée

---

## Session 2026-06-15 — Fix mise en page InspectionPanel + caméra absente dans Settings

### Contexte
Suite de la session 2026-06-14. Deux problèmes découverts lors du premier test visuel de l'app sur le Jetson :
1. **Mise en page** : la section « Export Report » (bas du dock Inspection) avait des boutons chevauchants/tronqués — le panel était trop haut pour le dock sans ascenseur.
2. **Camera dans Settings** : `SettingsDialog → Camera → Device` affichait « No camera detected » même quand la caméra était bien détectée dans le reste de l'app.

### Cause de la mise en page
`InspectionPanel::buildUI()` installe directement un `QVBoxLayout` sur `this`. Avec 4 groupes (Inspection/Measure/Snapshots/Export) + Progress/Labels/Buttons, le panel dépasse la hauteur du dock → Qt compresse les widgets du bas. Référence : `ControlPanel` utilise déjà le pattern `QScrollArea` + widget contenu.

### Fix 1 — InspectionPanel : scroll area (`src/gui/InspectionPanel.cpp`)
Même pattern que `ControlPanel` :
```
QScrollArea (this, widgetResizable, NoFrame)
  └─ content (QWidget)
       └─ QVBoxLayout — contient les 4 groupes
```
Includes ajoutés : `<QScrollArea>`, `<QFrame>`.

### Fix 2 — SettingsDialog : V4L2 enumeration (`src/gui/SettingsDialog.cpp`)
`SettingsDialog::enumerateCameras()` utilisait `QMediaDevices::videoInputs()` (aveugle sur Jetson/Docker) → même cause que l'erreur #17 dans Application.cpp. Fix : remplacé par `ibom::camera::CameraCapture::listDevices()` (V4L2 OpenCV), `QMediaDevices` conservé uniquement pour les libellés. Include `CameraCapture.h` ajouté.

### Fix 3 — MainWindow : corner ownership (`src/gui/MainWindow.cpp`)
La capture d'écran montrait que le dock **Inspection** (gauche) et le dock **Statistics** (bas) se chevauchaient dans le coin bas-gauche. Par défaut Qt attribue les deux coins du bas au `BottomDockWidgetArea` → le dock Statistics s'étend pleine largeur **sous** le dock Inspection, et quand le contenu du panel débordait (problème Fix 1) il se dessinait par-dessus. Ajout dans `createDockWidgets()` :
```cpp
setCorner(Qt::BottomLeftCorner,  Qt::LeftDockWidgetArea);
setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
```
→ les docks gauche/droite occupent toute la hauteur, le dock bas reste dans la colonne centrale. Layout type IDE, plus de conflit de coin.

### Fichiers modifiés
- `src/gui/InspectionPanel.cpp` — QScrollArea pattern
- `src/gui/SettingsDialog.cpp` — V4L2 enumeration + include CameraCapture.h
- `src/gui/MainWindow.cpp` — setCorner (bottom corners → side dock areas)

### ⚠️ À valider sur le Jetson
```bash
bash scripts/build_jetson.sh && cd build && ctest --output-on-failure
```
Tests manuels :
- Dock Inspection : faire défiler jusqu'aux boutons Report HTML/PDF (scroll doit apparaître si la fenêtre est trop petite)
- Settings → Camera : onglet ouvert → Device doit afficher « 0: /dev/video0 » au lieu de « No camera detected »

### Fix 4 — nouveau script `scripts/run_dev_shell.sh`
L'utilisateur ne savait jamais quel script lancer pour avoir un **shell dev AVEC caméra**.
Cause : `docker/run-dev.sh` n'utilise que `compose.yml` (pas de `/dev/video*`), et
`run_local_gui.sh` lance le binaire directement (pas de shell). Aucun ne faisait
« shell dev + vidéo ».

Nouveau `scripts/run_dev_shell.sh` : même setup que `run_local_gui.sh` (override caméra
dynamique `/tmp/microscope-ibom.cameras.yml` + X11 + xauth) mais dépose dans `bash` au
lieu de lancer le binaire. README Docker corrigé (tableau des 3 scripts ; l'ancien README
prétendait à tort que `run-dev.sh` donnait accès aux caméras).

### Fix 5 — Defect Log → Event Log (logs runtime visibles dans l'UI)
L'utilisateur signalait que le « Defect Log » était toujours vide (il ne se remplissait
que via la détection IA de défauts, non câblée sans modèle) et voulait y voir les logs
qui posent problème + les logs de calibration, etc.

- **`src/utils/QtLogSink.{h,cpp}`** (nouveau) : `LogBridge` (QObject singleton, signal
  `messageLogged(int level, QString logger, QString message)`) + `qt_signal_sink<Mutex>`
  (sink spdlog qui pousse chaque record vers LogBridge ; thread-safe, marshalé sur le
  thread GUI via QueuedConnection).
- **`src/utils/Logger.cpp`** : ajout du `qt_signal_sink_mt` (niveau **info+**) aux sinks —
  garde les événements de cycle de vie (caméra, calibration, iBOM, AI) + tous les
  warnings/erreurs, sans le spam debug/trace par frame.
- **`src/gui/StatsPanel.{h,cpp}`** : « Defect Log » renommé **« Event Log »**, colonnes
  Time | Level | Message. Nouveau slot `addLogEntry(level, logger, message)` (couleur par
  niveau : rouge=ERR/CRIT, orange=WARN, gris=INFO). `addDefectEntry()` réécrit dessus
  (level DEFECT, ref stockée en UserRole → clic = navigation conservée). Log borné à 500
  lignes (drop des plus anciennes).
- **`src/app/Application.cpp`** : connexion `LogBridge::messageLogged` → `StatsPanel::addLogEntry`
  (QueuedConnection) dans `connectSignals()`.
- **`CMakeLists.txt`** : ajout de `QtLogSink.{h,cpp}`.

### Fix 6 — défilement tactile (écran Minix) sur le panneau gauche
L'utilisateur (écran tactile Minix SF16T) signalait que « les ascenseurs du panneau de
gauche ne fonctionnent pas ». Cause : ascenseurs Qt à 8px (impossibles au doigt) + pas
de glisser-pour-défiler tactile sur `QScrollArea` par défaut.

- **`src/gui/InspectionPanel.cpp`** : `QScroller::grabGesture(scroll->viewport(),
  QScroller::LeftMouseButtonGesture)` → défilement cinétique au doigt (le tactile X11 est
  synthétisé en souris, donc LeftMouseButtonGesture couvre tactile + souris ; les taps
  passent quand même en clic). InspectionPanel n'a pas de slider → pas de conflit.
- **`src/gui/MainWindow.cpp`** : ascenseurs élargis 8px → **14px** (handle radius 7px,
  min 48px) dans les deux thèmes (dark + light) — saisissables au doigt partout.
- **ControlPanel** (droite) : volontairement **sans** QScroller (slider d'opacité +
  spinbox → le cinétique intercepterait le glisser). Bénéficie quand même de l'ascenseur
  14px.

### Reste à faire
- [ ] Valider calibration `findChessboardCornersSB` (fix erreur #18 — déjà pushé, test demain)
- [ ] Segfault à la fermeture (RemoteView teardown, non bloquant)
- [ ] Ouvrir PR `claude/pr5-plus-fixes` → `main` quand tous les tests passent

---

## Session 2026-06-12 (suite 3) — Restauration session d'inspection + rapports HTML/PDF

### Contexte
GO utilisateur (« go 1 et 2 ») sur les deux items recommandés du backlog restant.

### Livré
1. **Restauration de session d'inspection (item 3.3)** :
   - `Application::saveInspectionState()` / `loadSavedPlacedRefs()` : fichier `$IBOM_DATA_DIR/session_state.json`, objet JSON clé = **chemin iBOM** → `{placed: [...], saved_at: ISO}`. Set vide = entrée supprimée. Lecture tolérante (JSON corrompu → repart de zéro).
   - Sauvegarde à **chaque clic « Placed »** (fichier minuscule, robuste aux coupures) et au **Reset** (qui efface l'entrée).
   - Reprise à deux niveaux : au **chargement de l'iBOM** (overlay + BomPanel montrent immédiatement les composants déjà posés) et au **Start Inspection** — nouveau `PickAndPlace::restorePlaced(unordered_set)` : marque les steps, se positionne sur le premier non-placé, émet progress/currentStep/allPlaced une seule fois. Statut « Inspection resumed: N/M already placed ».
   - Si les refs sauvées ne matchent rien (autre carte au même chemin) → départ propre.
2. **Rapports HTML/PDF (item 2.2)** :
   - `Application::onExport()` : nouveaux formats `report-html` / `report-pdf` — construit les `InspectionResult` depuis les mêmes records que les exports CSV (statut placed/pending), config rapport depuis `boardInfo` (titre + révision), snapshot caméra via `CameraView::captureView()`, puis `ReportGenerator::generateHTML/generatePDF` (libharu, guard `__has_include`).
   - `InspectionPanel` : 2 boutons « Report HTML » / « Report PDF » (ligne 3 de la grille d'export).
   - `MainWindow::onExportReport` (action **File → Export Report**, Ctrl+E) génère maintenant le **vrai rapport** HTML au lieu d'un simple CSV.

### Fichiers modifiés
`src/app/{Application.h,Application.cpp}`, `src/features/{PickAndPlace.h,PickAndPlace.cpp}`, `src/gui/{InspectionPanel.h,InspectionPanel.cpp,MainWindow.cpp}`, `docs/IDEES_AMELIORATIONS.md`, `docs/JETSON_SESSION_LOG.md`.

### ⚠️ À valider sur le Jetson (rien compilé ici)
```bash
bash scripts/build_jetson.sh && cd build && ctest --output-on-failure
```
Tests manuels : inspecter quelques composants → fermer l'app → relancer (auto-reload iBOM) → Start Inspection doit reprendre au bon endroit ; bouton Report HTML doit produire un rapport ouvrable ; Report PDF dépend de libharu (présent dans l'image base).

### Prochaine étape
1. Build + ctest Jetson (toute la série de commits du jour)
2. Backlog restant : BarcodeScanner + assoc iBOM (2.3), fin onglet Features (dropdown modèle, dark mode), cheat-sheet raccourcis, InferenceWorker async (dès modèle v1)

---

## Session 2026-06-12 (suite 2) — Items 4-6 : focus assist, fichiers récents, RemoteView

### Contexte
Suite du GO utilisateur (« oui continue ») → items 🟠 4-6 de la priorisation d'[IDEES_AMELIORATIONS.md](IDEES_AMELIORATIONS.md), plus l'essentiel de l'item 7 (onglet Features).

### Livré
1. **Focus assist (item 3.1)** :
   - `StatsPanel::setSharpness(double, bool)` + ligne « Focus: » dans la grille Performance (vert = au-dessus du seuil `dataset.min_sharpness`, orange sinon ; tooltip mode d'emploi).
   - `Application` : calcul dans le handler `frameReady`, **throttlé 300 ms**, variance Laplacien via `ImageUtils::computeSharpness` sur downscale 0.25× (même échelle que le gate du DatasetCreator → la valeur affichée est directement comparable au seuil 100).
2. **Fichiers récents + auto-reload (item 3.2)** :
   - `Config` : `recentIbomFiles()` (max 5, dédup, plus récent en tête), `addRecentIbomFile()`, `autoReloadIbom` (défaut **true**) — clés JSON `ibom_recent` / `ibom_auto_reload`.
   - `MainWindow` : sous-menu **File → Open Recent** (chemins complets — les iBOM s'appellent souvent tous `ibom.html`), désactivé si vide.
   - `Application` : `refreshRecentFilesMenu()` ; succès de `loadIBomFile` → push dans la config + refresh ; au démarrage → auto-reload du dernier iBOM si le fichier existe.
3. **RemoteView câblé (item 2.1)** :
   - Préalable viewer : le serveur ne parle que WebSocket (pas HTTP) et le HTML généré utilisait `location.host` (inutilisable en `file://`). Fix dans `RemoteView` : hôte/port lus depuis l'URL (`remote_view.html?host=<ip>&port=<p>`), fallback port injecté via placeholder `__WS_PORT__` ; `generateHTMLViewer()` passé en public.
   - `Application::applyRemoteViewConfig()` : start/stop/restart selon `features.remote_view`(+port), écrit le viewer dans `$IBOM_DATA_DIR/remote_view.html`, log le mode d'emploi. Appelé à l'init **et** sur `settingsChanged`.
   - Push des frames dans le handler `frameReady` **seulement si des clients sont connectés** (throttle 15 fps côté RemoteView). ⚠ La frame envoyée = image caméra sans overlay (overlay composé séparément dans CameraView) — composition remote = amélioration future.
   - Au passage : la copie `qimg.copy()` est maintenant partagée (COW) entre CameraView et RemoteView — pas de 2ᵉ copie.
4. **Onglet Settings → Features (item 3.4 partiel)** : case remote view + port (1024-65535) + case « Reload last iBOM at startup ». `settingsChanged` applique aussi maintenant la confiance IA au détecteur et resynchronise le spinner du ControlPanel. *Restent pour plus tard : dropdown detector model, dark mode, checkbox columns.*

### Fichiers modifiés
`src/app/{Application.h,Application.cpp,Config.h,Config.cpp}`, `src/gui/{MainWindow.h,MainWindow.cpp,StatsPanel.h,StatsPanel.cpp,SettingsDialog.h,SettingsDialog.cpp}`, `src/features/{RemoteView.h,RemoteView.cpp}`, `docs/IDEES_AMELIORATIONS.md`, `docs/JETSON_SESSION_LOG.md`.

### ⚠️ À valider sur le Jetson (rien compilé ici)
```bash
bash scripts/build_jetson.sh && cd build && ctest --output-on-failure
```
Tests manuels : ligne « Focus » réagit à la molette de mise au point ; File → Open Recent après un premier chargement ; Settings → Features → cocher remote view → ouvrir `~/ibom-data/remote_view.html?host=<ip-jetson>` depuis le PC (port 8080 accessible direct grâce à `network_mode: host`).

### Prochaine étape
1. Build + ctest Jetson
2. Reste du backlog : restauration session d'inspection (3.3), InferenceWorker async (3.5, dès le modèle v1), bouton rapport (2.2), BarcodeScanner+assoc iBOM (2.3)

---

## Session 2026-06-12 (suite) — Implémentation des 3 quick wins

### Contexte
GO utilisateur (« tu peux commencer à faire les modifications ») → implémentation des priorités 🔴 1-3 de [IDEES_AMELIORATIONS.md](IDEES_AMELIORATIONS.md).

### Livré
1. **Statut IA visible (item 1.3)** :
   - `MainWindow` : nouveau label permanent `m_aiLabel` dans la status bar (« AI: -- » au départ), méthode `updateAiStatus(bool ready, QString)` — vert (`placedCSS`) quand prêt, orange (`defectCSS`) sinon, tooltip = message complet.
   - `Application::connectSignals()` : `aiStatusChanged` → `MainWindow::updateAiStatus` (connexion **avant** `initializeAI()`, donc le premier « Loading AI model… » est capté ; les émissions du thread d'init arrivent en queued automatiquement).
   - `initializeAI()` : les retours anticipés émettent maintenant aussi le statut (« AI: disabled », « AI: no model ») au lieu de laisser le label muet.
2. **Garde-fou décompression LZString (item 1.1)** — `IBomParser.cpp` :
   - Nuance vs l'analyse : la boucle `while(true)` a déjà une sortie (`data.index > length`), le vrai risque est l'**expansion quadratique** (zip-bomb LZ78) sur données corrompues → OOM avant la fin du flux.
   - Fix : borne `maxOutputChars = 1000 × taille_entrée + 1 MiB` vérifiée après chaque `result += entry` → `spdlog::error` + `nullopt`.
   - `decompressLZString` passé en **public** dans `IBomParser.h` pour test direct.
   - `tests/test_ibom_parser.cpp` : nouveau TEST_CASE (entrée vide, 50 000 chars de base64 pseudo-aléatoire déterministe → doit terminer, caractères non-base64 → pas de crash). Borne du test ×3 (sortie UTF-8 vs garde UTF-16).
3. **Slider de confiance câblé (item 1.2)** :
   - `Application::connectSignals()` : `ControlPanel::confidenceChanged` → `m_config->setDetectionConfidence()` + `setConfidenceThreshold()` sur le détecteur s'il est prêt (`componentDetector()` null-safe).
   - `ControlPanel::setConfidenceThreshold(float)` (nouveau, avec `QSignalBlocker`) ; appelé dans `Application::initialize()` pour refléter la valeur persistée au démarrage (spinner borné 0.1–1.0).
4. **Bonus découvert en route** : `Config::save()` n'était appelé **que** par le SettingsDialog — tous les réglages faits via le ControlPanel (opacité, caméra, confiance…) étaient perdus à la fermeture. Fix : `m_config->save()` en tête de `~Application()`.

### Fichiers modifiés
`src/gui/MainWindow.{h,cpp}`, `src/gui/ControlPanel.{h,cpp}`, `src/app/Application.cpp`, `src/ibom/IBomParser.{h,cpp}`, `tests/test_ibom_parser.cpp`, `docs/IDEES_AMELIORATIONS.md` (statut), `docs/JETSON_SESSION_LOG.md`.

### ⚠️ À valider sur le Jetson (rien compilé ici — pas de toolchain)
```bash
bash scripts/build_jetson.sh
cd build && ctest --output-on-failure
```
Vérifier au lancement : label « AI: no model » (orange) dans la status bar tant que `models/` est vide.

### Note infra (session)
Le push git direct via le proxy de session renvoie 403 (token lecture seule) — contourné avec un PAT fourni par l'utilisateur. **Le PAT a transité en clair dans la conversation : à révoquer après la session** (Settings GitHub → Developer settings → Tokens).

### Prochaine étape
1. Build + ctest sur le Jetson (8 suites attendues : les 7 existantes + le test LZString étendu dans test_ibom_parser)
2. Puis items 🟠 4-7 du backlog (focus assist, fichiers récents, RemoteView, onglet Settings « Features ») selon priorité utilisateur

---

## Session 2026-06-12 — Analyse améliorations & nouvelles features (IDEES_AMELIORATIONS.md)

### Contexte
Demande utilisateur : « trouve des améliorations et des nouvelles features que je pourrais ajouter, fais-moi un journal ». Session d'**analyse pure** (environnement sans toolchain — rien compilé, rien modifié dans le code).

### Méthode
Exploration complète de `src/`, `tools/`, `tests/`, `Config.h` vs `SettingsDialog`, modules instanciés vs dormants dans `Application.cpp` — en excluant tout ce qui est déjà planifié (Phases B/C/D dataset, Phase 2d/2.5/3, audit [JETSON_AMELIORATIONS.md](JETSON_AMELIORATIONS.md), roadmap [PROCHAINE_SESSION.md](PROCHAINE_SESSION.md)).

### Livré
- **Créé : [docs/IDEES_AMELIORATIONS.md](IDEES_AMELIORATIONS.md)** — propositions priorisées en 4 catégories :
  1. **Quick wins/risques** : `while(true)` sans garde dans la décompression LZ-String (`IBomParser.cpp:389` — freeze/OOM possible sur HTML corrompu), slider de confiance `ControlPanel` jamais connecté au détecteur, signal `aiStatusChanged` jamais affiché en GUI, micro-perfs (`qimg.copy()` par frame, `SetIntraOpNumThreads(4)` hardcodé).
  2. **Features dormantes** (code ~complet, jamais instancié) : RemoteView (WebSocket MJPEG — idéal atelier Jetson), ReportGenerator, BarcodeScanner (+ idée combo : association code-barres → iBOM auto-chargé), StencilAlign, OCR/SolderInspector (attendent des modèles), VoiceControl (squelette).
  3. **Nouvelles features** : focus assist (netteté Laplacien live — métrique déjà calculée par DatasetCreator), fichiers récents + auto-reload iBOM, restauration de session d'inspection (`m_placedRefs` perdus au restart), onglet Settings pour les 6 clés config sans UI, **InferenceWorker async** (prérequis du point F roadmap — `detect()` n'est invoqué nulle part et bloquerait le thread GUI), cheat-sheet raccourcis, i18n FR.
  4. **Hygiène** : test round-trip `Config` manquant, code mort `estimateOrientation()`, TODO arcs silkscreen (`OverlayRenderer.cpp:206`), `TODO.md` racine obsolète (2026-03-20, ère Windows).
- Recommandation : faire les 3 quick wins (~1 h 30) **avant** la première session de capture dataset ; la priorité produit reste calibration → capture → entraînement v1.

### Fichiers
- **Créé** : `docs/IDEES_AMELIORATIONS.md`
- **Modifié** : `docs/JETSON_SESSION_LOG.md` (cette entrée + bloc État actuel)

### Prochaine étape
1. Décision utilisateur sur la priorisation (§5 du doc) — quels items implémenter en premier
2. Inchangé côté produit : calibration caméra + première capture dataset ([PROCHAINE_SESSION.md](PROCHAINE_SESSION.md))

---

## Session 2026-06-14 — Test PR #5 sur Jetson : fix link libharu

### Contexte
L'utilisateur teste la PR #5 (`l0l0l0/Assistant:claude/focused-fermi-if21mm`, 9 items de
`IDEES_AMELIORATIONS.md`) sur le Jetson, dans le container dev, depuis un autre compte
GitHub. Marche à suivre établie : fork ajouté comme remote `pr5`, fetch + checkout de la
branche dans le container (réseau GitHub OK seulement depuis le container, pas l'hôte
Jetson), puis `bash scripts/build_jetson.sh`.

### Problème rencontré
Build KO au link : `undefined reference to HPDF_*` (cf [JETSON_ERREURS.md](JETSON_ERREURS.md)
entrée #16). `ReportGenerator.cpp` compile le code PDF via `__has_include(<hpdf.h>)` (header
présent) mais le CMake ne linke jamais `libhpdf.so` (aucun `find_package` n'aboutit avec
le paquet apt `libhpdf-dev` qui ne fournit pas de config CMake).

### Livré
- **`CMakeLists.txt`** : fallback `find_library(HPDF_LIBRARY NAMES hpdf)` + branche de link
  `elseif(HPDF_LIBRARY)`. Aligne CMake sur le critère `__has_include` du `.cpp`.
- Entrée #16 dans `JETSON_ERREURS.md` (🟡 CONTOURNÉ — validation build Jetson en attente).

### Validé sur Jetson (2026-06-14)
- ✅ Rebuild OK après le fix CMake — binaire `build/bin/MicroscopeIBOM` (1,4 MB)
- ✅ `ctest --output-on-failure` : **7/7 tests passent** (0,45 s)
- Erreur #16 passée ✅ RÉSOLU.
- ⚠️ Piège rencontré : copier-coller multi-lignes dans le shell du container pollue le
  buffer d'entrée (commandes avalées, sortie absente, syntax errors fantômes). Solution :
  ressortir/rentrer dans le container et taper les commandes **une par une**.

### Suite — caméra non détectée (erreur #17)
Après build OK, lancement de l'app : caméra HAYEAR MOS-4K Pro branchée, `/dev/video0` mappé
dans le container, OpenCV l'ouvre (test Python `True True`), `v4l2-ctl` liste MJPG
1920×1080@30 — **mais** l'app logue `Found 0 camera(s)` et l'UI ne montre rien.

**Cause** : `Application.cpp` énumérait via `QMediaDevices::videoInputs()` (Qt Multimedia,
aveugle aux `/dev/video*` sur Jetson/Docker) alors que la capture ouvre par index en
OpenCV/V4L2. `CameraCapture::listDevices()` (OpenCV) existait mais n'était pas utilisée.

**Fix appliqué** : énumération via `camera::CameraCapture::listDevices()`, QMediaDevices
réduit à un simple fournisseur de libellé. Cf [JETSON_ERREURS.md](JETSON_ERREURS.md) #17.

Piège de chemin de log noté au passage : le log courant est `logs/pcb_inspector.log`
**relatif au CWD de lancement** (pas `build/bin/logs/`). Lancé depuis `/opt/microscope-ibom`
→ `/opt/microscope-ibom/logs/pcb_inspector.log`.

### ✅ Validé sur Jetson (2026-06-14)
- `Found 1 camera(s) (V4L2 enumeration)` — caméra HAYEAR MOS-4K Pro détectée
- `Camera opened: 1920x1080 @ 30 fps, FOURCC=MJPG` — flux affiché à l'écran ✅
- Erreur #17 → ✅ RÉSOLU

### Reste à faire
- [ ] **SettingsDialog** : la caméra n'apparaît pas dans l'onglet Settings (QMediaDevices
      encore utilisé là). Cosmétique — même fix que Application.cpp à appliquer dans SettingsDialog.cpp.
- [ ] **Calibration** : `No checkerboard patterns detected` — pas un bug code, le patron
      doit être imprimé et présenté correctement à la caméra (Menu Aide → patron).
- [ ] **Segfault à la fermeture** : `exiting with code 0` puis `Segmentation fault` — teardown
      Qt (probablement RemoteView). Non bloquant.
- [ ] Ouvrir PR `claude/pr5-plus-fixes` → `main` (branche déjà prête).

---

## Session 2026-06-11 (suite) — Phase A : capture + annotation auto (DatasetCreator)

### Contexte
GO utilisateur sur le plan Phase A de [DATASET_CREATOR_PLAN.md](DATASET_CREATOR_PLAN.md), défauts validés : boxes droites (pas OBB), JPEG q95, résolution native 1920×1080.

### Livré
- **TrackingWorker** : signal `homographyUpdated(cv::Mat)` → `homographyUpdated(cv::Mat, int inliers, double reprojErrPx)` — inliers depuis le mask RANSAC, erreur = **médiane** des reprojections des inliers. Connexion existante + test mis à jour (vérifie inliers ≥ 15 et err < 3px sur warp synthétique).
- **`resources/footprint_classes.json`** : 14 classes (même liste ordonnée que `tools/dataset_studio/config/pcb_classes.json` — contrat Studio), règles regex ordonnées (footprint/ref/value, AND entre champs présents, OR = plusieurs règles). Règles LED avant diode, WS2812/SK6812 → led. Validé par simulation Python sur 17 footprints KiCad réels.
- **`src/features/DatasetCreator.{h,cpp}`** : QThread dédié (pattern TrackingWorker). 5 gates (inliers≥25, reproj≤3px, netteté Laplacien≥100 sur downscale 0.25×, exposition ≤5% pixels cramés/noirs, fraîcheur homographie ≤300ms), throttle 500ms, anti-doublon de pose (coins board projetés, Δ moyen ≥15px), projection bboxes (filtre Layer, shrink 0.85, clip ≥60% visible, min 12px), sortie `$IBOM_DATA_DIR/dataset/session_<date>_<board>_<éclairage>/` (images JPEG q95 + labels YOLO + manifest.jsonl avec homographie/inliers/blur/tags). Footprints non mappés loggés + récapitulés au stop. `ClassMapper` + `projectLabels()` purs → testables.
- **`src/gui/DatasetPanel.{h,cpp}`** : dock gauche (tabifié avec Inspection) — board/éclairage, Start/Stop, 5 gates ●vert/rouge avec valeurs, compteurs sauvées/rejetées **par cause**, labels dernière frame.
- **Config** : section `dataset.*` (10 seuils, defaults plan §A2/A3).
- **Application** : thread + worker + 8 connexions (frames bruts — cohérents avec l'homographie tracking estimée sur frames bruts), `setProject` au chargement iBOM (Layer::Front v1). ⚠ Metatypes : slot `setProject` déclaré avec types **pleinement qualifiés** (piège #17).
- **`tests/test_dataset_creator.cpp`** : 6 cas — règles ordonnées/insensibles à la casse, rejet sans 'other', géométrie YOLO exacte (scale ×10), shrink, gates (layer/taille/clipping), homographie vide.

### ✅ Build Jetson validé (2026-06-11)
`bash scripts/build_jetson.sh` sur Jetson AGX Orin JP6.2 container dev :
- **31/31 steps**, binaire `build/bin/MicroscopeIBOM` (1.3 MB) généré sans erreur
- Qt 6.2.4 / OpenCV 4.10.0 / CUDA / TensorRT / UMA tous détectés
- 2 warnings `-Wunused` dans `Application.cpp` (lignes 474 et 520) — corrigés avec `[[maybe_unused]]` dans le commit suivant
- `ctest --output-on-failure` : ✅ **7/7 tests passent** (0.48 s total)

### ✅ Branche validée — mergée dans main
`claude/dreamy-cori-oec93c` → `main` après validation complète sur Jetson.

### Prochaine étape
1. Première session de capture réelle (caméra branchée + carte avec iBOM, onglet Dataset dans l'app)
2. Phase B (assistant variété : carte de couverture, check-list zoom/éclairage, quotas)
3. Phases C1-C2 (validate_dataset.py, split_dataset.py) si pas déjà faits

---

## Session 2026-06-11 — Merge Lots 1+2 dans main + fix INSTALL.bat

### Contexte
PR #2 (Lots 1+2 Dataset Studio + tout le travail Jetson de la branche `claude/dreamy-cori-oec93c`) **mergée dans `main`** (`f36895e`). L'utilisateur teste sur son PC Windows et rapporte : `INSTALL.bat` ne trouve pas Python.

### Bug trouvé
`INSTALL.bat` ligne 7 : `where python >/dev/null 2>&1` — syntaxe **Unix** invalide en cmd.exe (il faut `>nul`). La redirection échoue → le test python échoue toujours → message "Python introuvable" même avec Python installé. Erreur de ma part lors du Lot 1 (jamais testé sur un vrai Windows).

### Fix livré
`INSTALL.bat` réécrit :
- Détection via le lanceur **`py -3`** d'abord (installé par python.org, marche même sans "Add to PATH"), fallback `python`
- Redirections `>nul 2>&1` (syntaxe cmd correcte)
- Check version explicite ≥ 3.10 (au lieu d'une exigence implicite 3.10–3.12)
- Codes d'erreur vérifiés sur `venv` et `pip install`
- Le venv reste créé automatiquement (une seule fois) — c'est voulu, START.bat le réutilise

### Ajout scripts Linux
L'utilisateur a un Ubuntu avec RTX 5070 Ti. Le code Python est 100% cross-platform (pathlib, tkinter, webbrowser). Ajoutés :
- `install.sh` : détection Python 3.10+ (python3.12/11/10/3), check tkinter, venv, pip
- `start.sh` : activate venv + python app.py
- `install_training.sh` : torch cu128 + ultralytics + gpu_check
- README.md mis à jour (Windows + Linux)

### Prochaine étape
Re-test utilisateur : `git pull origin main` → `INSTALL.bat` (Windows) ou `./install.sh` (Ubuntu) → lancer le wizard.

---

## Session 2026-06-10 (suite 7) — Lot 2 PCB Dataset Studio (split + entraînement)

### Contexte
GO utilisateur pendant qu'il teste le Lot 1 sur son PC. Périmètre = plan §6 Lot 2.

### Livré
- `studio/session_split.py` : split **par session** (jamais par image) — greedy plus-petites-sessions-en-val pour approcher le ratio, garde ≥1 session de chaque côté, avertit si une combinaison éclairage|zoom (manifest.jsonl) n'existe qu'en val ; fallback mono-session = split contiguë + warning. Sortie : `train.txt`/`val.txt` (chemins absolus, ultralytics retrouve les labels via `/images/`→`/labels/`) + `data.yaml` commenté
- `studio/gpu_check.py` : détection torch/CUDA/ultralytics + **piège Blackwell** — vérifie que `sm_120` est dans `torch.cuda.get_arch_list()` (un torch < 2.7 voit le GPU mais plante au premier kernel) ; exécutable en module (`python -m studio.gpu_check`)
- `studio/vendor/training_manager.py` : vendorisé de Pokemon (adaptations marquées [PCB] : défauts yolov8m/pcb_detector/degrees=180/flipud=0.5 + callback `on_fit_epoch_end` → progression par epoch dans le journal GUI)
- `app.py` : étapes 3 (ratio val + résumé du split persisté) et 4 (bandeau GPU async, presets rapide/standard/précis depuis defaults.json, overrides epochs/batch, confirmation si CPU, best.pt + métriques persistés dans le projet)
- `install_training.bat` : torch cu128 + ultralytics + check GPU auto

### Tests effectués (hors GUI, hors vrai GPU)
✅ py_compile · ✅ split 4 sessions (30/10, zéro intersection train/val, data.yaml valide) · ✅ fallback mono-session (8/2 + warning) · ✅ gpu_check sans torch (message clair) · ✅ TrainingManager avec **ultralytics factice** : kwargs vérifiés (degrees=180, flipud=0.5, data/epochs corrects), métriques remontées, callback epoch enregistré.
⚠️ Restent à tester par l'utilisateur sur le PC : la GUI des étapes 3-4 et un **vrai** entraînement (install_training.bat → preset rapide sur le dataset factice).

### Prochaine étape
Retours utilisateur Lots 1-2 → Lot 3 (test visuel, export ONNX via scripts/export_yolov8_onnx.py, import/déploiement scp Jetson)

---

## Session 2026-06-10 (suite 6) — Implémentation Lot 1 PCB Dataset Studio

### Décisions
- **Langage : Python confirmé** (question Rust posée par l'utilisateur → refusé avec argumentaire : ultralytics/PyTorch est Python-only, l'intérêt = réutiliser training_manager/validator existants ; Rust = tout réécrire + embarquer Python quand même)
- Test à blanc : **générateur de dataset factice** retenu

### Livré — `tools/dataset_studio/` (Lot 1 complet)
- `app.py` (~420 l.) : wizard Tkinter 6 étapes (0-2 actives, 3-5 placeholders), thème sombre, journal live (queue + after), opérations en thread (`_run_bg`)
- `studio/project.py` : config persistée `~/.pcb_dataset_studio/project.json` (workdir, accès Jetson pour Lot 3)
- `studio/import_manager.py` : scan/copie de sessions (`images/`+`labels/`), anti-écrasement
- `studio/fake_dataset.py` : générateur factice (faux PCB + composants rectangulaires, labels YOLO exacts par construction, manifest.jsonl avec tags)
- `studio/validation.py` : orchestre le validator par session, agrège les classes, rapports HTML, **mosaïque d'aperçu avec bboxes dessinées**
- `studio/vendor/` : `dataset_validator.py` (512 l.) + `safe_print` vendorisés de Pokemon-Dataset-Creator avec en-têtes d'attribution
- `config/pcb_classes.json` (14 classes, ⚠️ même liste ordonnée que la future Phase A), `config/defaults.json` (presets entraînement pour Lot 2)
- `INSTALL.bat`/`START.bat` (CRLF) avec avertissement **PyTorch ≥ 2.7/cu128 pour Blackwell sm_120**, `requirements.txt`, `README.md`

### Tests effectués (dans l'environnement d'analyse, hors GUI)
✅ py_compile de tous les modules · ✅ bout-en-bout : génération 2 sessions × 8 images → scan → import → ré-import ignoré → validation (0 erreur, 16 images, 14 classes) → 2 rapports HTML + mosaïque vérifiée visuellement (boxes alignées).
⚠️ **La GUI Tkinter n'a pas pu être lancée ici** (pas de display) — premier lancement réel = test utilisateur sur le PC Windows.

### À tester par l'utilisateur (PC fixe Windows)
1. `git pull` puis ouvrir `tools/dataset_studio/`, double-clic `INSTALL.bat` puis `START.bat`
2. Étape 0 : choisir le dossier de travail → Enregistrer
3. Étape 1 : « Générer » (dataset factice) · Étape 2 : « Lancer la validation » → ouvrir rapport + aperçu
4. Remonter tout problème (la GUI n'a jamais tourné)

### Prochaine étape
Retours utilisateur sur Lot 1 → **Lot 2** (split par session + entraînement, plan §6 du DATASET_STUDIO_PLAN)

---

## Session 2026-06-10 (suite 5) — Plan PCB Dataset Studio (wizard Windows)

### Objectif
L'utilisateur choisit de commencer par le **wizard Windows** (avant la Phase A Jetson). Conformément à la règle "plan avant action" : analyse du code réel de Pokemon-Dataset-Creator (désormais public, cloné en lecture) + plan détaillé.

### Analyse Pokemon-Dataset-Creator (code réel, pas le README)
- `core/` modulaire et largement **générique YOLO** : `training_manager.py` (412 l., TrainingConfig + callback log GUI), `dataset_validator.py` (507 l., rapport HTML), `auto_balancer_optimized.py` (356 l.), `dataset_exporter.py` (COCO/VOC), `utils.py` (safe_print Windows) → **réutilisés quasi tels quels** (vendor + attribution)
- Écarté : downloader/API cartes, holographic, mosaic (compositing cartes), prix
- Réécrit : GUI (8 642 lignes, trop spécifique — wizard neuf ~800 l. Tkinter) et **split** (le leur est aléatoire par image → interdit ici, fuite train/val entre frames d'une même session → `session_split.py` par session)

### Plan livré : [DATASET_STUDIO_PLAN.md](DATASET_STUDIO_PLAN.md)
- Wizard 6 étapes : Projet → Import (scp Jetson ou dossier) → Validation → Split/équilibrage → Entraînement (presets) → Test/Export ONNX/Déploiement Jetson
- Emplacement : `tools/dataset_studio/` ; 3 lots d'implémentation testables séparément
- ⚠️ Piège anticipé : **RTX 5070 Ti = Blackwell sm_120 → PyTorch ≥ 2.7 / CUDA 12.8** obligatoire (INSTALL.bat + check au démarrage)
- Indépendant de la Phase A (fonctionne sur tout dataset YOLO) → développable dès maintenant

### Fichiers
- **Créé** : `docs/DATASET_STUDIO_PLAN.md`

### Prochaine étape
GO utilisateur sur le plan → implémentation Lot 1 (squelette + import local + validation)

---

## Session 2026-06-10 (suite 4) — Fix caméra (devices container) + décisions dataset

### Contexte (retour utilisateur avec photo)
- Caméra microscope branchée : `lsusb` OK (`0ac8:3420 Z-Star Venus USB2.0 Camera`) mais app = "No camera detected" → devices commentés dans `compose.local.yml` (pas un bug, cf **erreur #15**)
- La photo montre l'**ancien** script (`(Re)demarrage...`) → l'utilisateur n'a pas encore pullé les commits des itérations 2-3 ; la validation build reste à faire
- **Pokemon-Dataset-Creator est désormais public** → analysé : app Python/Tkinter tout-en-un (augmentations imgaug, entraînement YOLOv8 un-clic, bbox_visualization, équilibrage de classes, INSTALL.bat/START.bat Windows)

### Décisions utilisateur
1. **Plan obligatoire avant toute action** — règle de travail demandée explicitement
2. GO fix caméra immédiat
3. **Wizard Windows "PCB Dataset Studio" dans le repo Assistant** (`tools/dataset_studio/`), adapté de Pokemon-Dataset-Creator — réutilise : structure GUI, pipeline imgaug, module entraînement/métriques ; remplace : téléchargement de cartes → import du dataset Jetson, classes Pokémon → classes PCB
4. Capture + annotation auto = **sur le Jetson** (Phase A du DATASET_CREATOR_PLAN) ; Windows = validation/augmentation/entraînement/export
5. Machine d'entraînement : **PC fixe RTX 5070 Ti 16 GB** (16 Go VRAM > 8 Go portable)

### Ce qui a été fait
- ~~`compose.local.yml` : `/dev/video0`+`/dev/video1` mappés en dur~~ → **remplacé le jour même** (l'utilisateur refuse qu'une caméra débranchée empêche le démarrage) par un **override dynamique** : `run_local_gui.sh` génère `/tmp/microscope-ibom.cameras.yml` avec les `/dev/video*` présents au lancement. Caméra absente = app démarre sans caméra ; caméra branchée = relancer le script. Hot-plug complet (cgroup rules + /dev monté) gardé en option B future. Détail : erreur #15 v2.
- `docs/JETSON_ERREURS.md` : entrée #15 (✅ RÉSOLU, v1 puis v2)

### À faire par l'utilisateur sur le Jetson (dans l'ordre)
```bash
cd ~/Assistant-git
git checkout docker/compose.local.yml && git pull
docker compose -f docker/compose.yml -f docker/compose.local.yml up -d dev   # recree le container (nouveaux devices)
docker compose -f docker/compose.yml -f docker/compose.local.yml exec dev bash scripts/build_jetson.sh
docker compose -f docker/compose.yml -f docker/compose.local.yml exec dev bash -c "cd build && ctest --output-on-failure"
docker compose -f docker/compose.yml -f docker/compose.local.yml exec dev v4l2-ctl -d /dev/video0 --list-formats-ext
bash scripts/run_local_gui.sh
```
Vérifier dans le log : `Camera opened: 1920x1080 @ 30 fps, FOURCC=MJPG`.

### Prochaines étapes (après validation caméra+build)
1. Phase A DATASET_CREATOR_PLAN (capture in-app) — **plan détaillé à présenter avant**
2. Wizard Windows `tools/dataset_studio/` — **plan détaillé à présenter avant**

---

## Session 2026-06-10 (suite 3) — Plan Dataset Creator (capture + annotation auto)

### Objectif
L'utilisateur veut un outil de constitution de dataset rapide (capture variée automatique + annotation automatique des composants, ~800 images), en réutilisant l'esprit de son projet `Pokemon-Dataset-Creator`. Livrable demandé : un plan exhaustif.

### Ce qui a été fait
- **Nouveau document : [DATASET_CREATOR_PLAN.md](DATASET_CREATOR_PLAN.md)** — plan complet en 4 phases :
  - **Idée centrale** : l'iBOM + l'homographie live = vérité terrain gratuite. Chaque frame où le tracking est verrouillé donne les bboxes de tous les composants projetées dans l'image (via `Homography::transformRect` + `ComponentMap::allComponents()`) → labels YOLO sans annotation manuelle.
  - **Phase A** (cœur) : mode capture intégré à l'app (`features/DatasetCreator` + `gui/DatasetPanel`) — gates qualité (inliers RANSAC ≥ 25, reproj ≤ 3 px, netteté Laplacien, exposition, fraîcheur homographie), anti-doublon de pose, projection + clipping + mapping footprint→classe (`footprint_classes.json`), writer YOLO, aperçu live des boxes.
  - **Phase B** : assistant de variété (carte de couverture via HeatmapRenderer, quotas zoom×éclairage, manifest.jsonl).
  - **Phase C** : outillage Python (validation visuelle, **split train/val par session** — jamais par image, review 5-10 % via Label Studio, augmentation offline optionnelle).
  - **Phase D** : boucle d'amélioration (hard-example mining = divergence modèle vs projection iBOM ; pré-annotation des cartes sans iBOM).
  - Risques/parades, ordre d'exécution chiffré, questions ouvertes (OBB, JPEG, résolution).
- **Préalable technique identifié** : `TrackingWorker::homographyUpdated` ne publie pas inliers/erreur de reprojection (log debug seulement) — signal à étendre en Phase A.

### Bloqueur d'analyse
`Pokemon-Dataset-Creator` est **privé** et hors du scope GitHub de la session (limité à `lo26lo/assistant`, outil d'ajout de repo indisponible) → section §8 du plan = hypothèses de réutilisation à confirmer quand l'utilisateur donnera accès au repo.

### Fichiers
- **Créé** : `docs/DATASET_CREATOR_PLAN.md`
- **Modifié** : `docs/JETSON_SESSION_LOG.md` (cette entrée)

### Prochaines étapes
1. (toujours en attente) **Valider le build Jetson** des itérations 2-3 (`build_jetson.sh` + ctest)
2. Décisions utilisateur sur §10 du plan (OBB ? JPEG ? résolution ?)
3. Implémenter la Phase A

---

## Session 2026-06-10 (suite 2) — Phase 1c + pipeline IA + runtime minimal

### Objectif
Suite au choix utilisateur (« ONNX ne sera jamais enlevé, que proposes-tu ? » → sélection : pipeline IA, Phase 1c, image runtime minimale) : implémenter ces trois chantiers.

### Décision actée
**ONNX Runtime ne sera jamais optionnel** — pas de build CI no-GPU. Pour une vraie CI C++, la voie retenue (non implémentée) est un runner GitHub Actions self-hosted sur le Jetson (build dans le container dev + ctest avec le stack complet).

### 1. Phase 1c — suppression du code Windows ✅
Plus **aucune** référence `_WIN32`/`Q_OS_WIN`/`IBOM_PLATFORM_WINDOWS`/MSVC dans `src/`, `cmake/`, `CMakeLists.txt` (repli = branche `windows-legacy`) :
- `main.cpp` : crash handler POSIX uniquement (SetUnhandledExceptionFilter supprimé)
- `CameraCapture.cpp` : backends V4L2+Auto uniquement (MSMF/DSHOW supprimés, 2 endroits)
- `InferenceEngine.cpp` : suppression de la conversion wstring Windows
- `SettingsDialog.cpp` : suppression init COM (3 blocs `Q_OS_WIN`)
- `utils/Paths.{h,cpp}` : branche `%APPDATA%` supprimée
- `CMakeLists.txt` : bloc `CMAKE_TRY_COMPILE_TARGET_TYPE` (piège Windows) supprimé ; bloc platform → `IBOM_PLATFORM_LINUX` inconditionnel
- `cmake/CompilerFlags.cmake` : branche MSVC supprimée
- Supprimés : `build_windows.bat`, `scripts/install_prerequisites.bat` (+ refs `.dockerignore`)

### 2. Pipeline IA câblé ✅ (nouveau doc : [AI_PIPELINE.md](AI_PIPELINE.md))
- `Config` : `ai.enabled` (défaut true) + `ai.detector_model` (défaut `component_detector`) — load/save JSON
- `Application::initializeAI()` (appelé depuis `initialize()`) : si un `.onnx` est trouvé par `ModelManager`, init ONNX Runtime + chargement modèle + création `ComponentDetector` **sur un `std::thread` dédié** (la compilation de l'engine TRT au 1er lancement prend des minutes — la GUI n'est pas bloquée). Sans modèle : log info, app 100 % fonctionnelle.
- Publication thread-safe : `m_aiReady` atomic ; accesseur `componentDetector()` (nullptr tant que pas prêt) ; signal `aiStatusChanged(bool, QString)` (à consommer par la GUI plus tard — pas encore affiché)
- Thread joiné dans `~Application()`
- `scripts/export_yolov8_onnx.py` : export YOLOv8 .pt → ONNX aligné sur le contrat de `InferenceEngine::preprocess` (640×640 FP32 statique, opset 17, FP16 délégué au TRT EP) + génération du `.txt` de classes
- `docs/AI_PIPELINE.md` : dataset (snapshots), annotation, entraînement YOLOv8m, export, déploiement, critères d'acceptation

### 3. Image runtime minimale ✅ (⚠️ jamais buildée)
`runtime.Dockerfile` réécrit : `FROM l4t-jetpack` (plus `FROM base`) + paquets **runtime uniquement** (libqt6*, gstreamer, libtbb12, OpenGL runtime…) + `COPY --from=microscope-ibom:base /usr/local/lib` (OpenCV CUDA, ORT, realsense, ZXing) + check `ldd | grep "not found"` qui **fail le build** si une dépendance manque. Les noms de paquets Jammy arm64 sont à valider au premier build (noté en tête du Dockerfile — logger tout écart dans JETSON_ERREURS.md).

### Fichiers (cette itération)
- **Créés** : `docs/AI_PIPELINE.md`, `scripts/export_yolov8_onnx.py`
- **Supprimés** : `build_windows.bat`, `scripts/install_prerequisites.bat`
- **Modifiés** : `src/main.cpp`, `src/camera/CameraCapture.cpp`, `src/ai/InferenceEngine.cpp`, `src/gui/SettingsDialog.cpp`, `src/utils/Paths.{h,cpp}`, `src/app/{Config.h,Config.cpp,Application.h,Application.cpp}`, `CMakeLists.txt`, `cmake/CompilerFlags.cmake`, `docker/runtime.Dockerfile`, `.dockerignore`, `CLAUDE.md` (statut ai/), `docs/JETSON_AMELIORATIONS.md` (§0)

### À valider sur le Jetson (rien compilé ici)
1. `git checkout docker/compose.local.yml && git pull`
2. `bash scripts/build_jetson.sh` puis `cd build && ctest --output-on-failure`
3. Optionnel : `docker compose -f docker/compose.yml build runtime` (première vraie tentative — s'attendre à devoir ajuster 1-2 noms de paquets)

---

## Session 2026-06-10 (suite) — Implémentation des améliorations de l'audit

### Objectif
Implémenter les recommandations de [JETSON_AMELIORATIONS.md](JETSON_AMELIORATIONS.md) (l'utilisateur a demandé « lance les améliorations »).

### ⚠️ Limite importante
Environnement d'analyse **sans toolchain** (pas de CUDA/Qt/OpenCV/ONNX) → le code C++ et le nouveau test **n'ont pas été compilés ici**. Tout a été écrit pour être correct à la relecture, mais **doit être validé sur le Jetson** : `bash scripts/build_jetson.sh` puis `cd build && ctest --output-on-failure`. Validés localement : `bash -n` des scripts, parse YAML de compose + workflow CI.

### Ce qui a été fait

**Persistance unifiée (prio 1 — `IBOM_DATA_DIR`)**
- Nouveau helper `src/utils/Paths.h/.cpp` : `ibom::utils::dataDir()` honore `$IBOM_DATA_DIR`, sinon `%APPDATA%/MicroscopeIBOM` (Win) ou `~/.local/share/MicroscopeIBOM` (Linux). Ajouté à `SOURCES`/`HEADERS` du CMakeLists.
- `Config.cpp::defaultConfigPath()` → `dataDir()/config.json` (suppression du `#ifdef` `~/.config`).
- `Application.cpp` → calibration (load+save) et snapshots passent par `dataDir()` au lieu de `QStandardPaths::AppDataLocation` (3 usages). Exports Documents/Pictures inchangés.
- `InferenceEngine.cpp` → cache TRT en chemin **absolu** `dataDir()/tensorrt-cache` (string local maintenu vivant jusqu'au `AppendExecutionProvider_TensorRT`).
- `compose.yml` : `IBOM_DATA_DIR=/opt/microscope-ibom/data` (dev + runtime) + volume unique `../data:/opt/microscope-ibom/data` (remplace `../config` et `../tensorrt-cache`). `runtime.Dockerfile` VOLUME mis à jour. `entrypoint.sh` crée `DATA_DIR`. `data/` ajouté à `.gitignore` + `.dockerignore`.

**Caméra (prio 2)** : `CameraCapture.cpp` demande `CAP_PROP_FOURCC=MJPG` avant la résolution + log du FOURCC réellement obtenu.

**Docker/scripts**
- `.dockerignore` : exceptions `!build/bin/MicroscopeIBOM` + `!docker/entrypoint.sh` (débloque `build runtime`).
- `compose.yml` runtime : retrait `QT_QPA_GENERIC_PLUGINS`/evdevtouch (doublons tactile sous xcb) ; `restart: on-failure:3` au lieu de `unless-stopped`.
- `entrypoint.sh` : check GPU adapté Tegra (`/dev/nvhost-gpu` avant `nvidia-smi`) ; bloc engines TRT réécrit (honnête, plus de faux « 5-15 min » à chaque boot).
- `run_local_gui.sh` : retrait `--force-recreate` ; cookie xauth réel (`xauth nlist | nmerge`) ; mode debug → `build-debug/bin`.

**CMake** : `cmake/CompilerFlags.cmake` — `IBOM_TARGET_CPU` (défaut `-march=native`, override `-mcpu=cortex-a78ae` pour build reproductible/portable).

**Tests (prio 6)** : `tests/test_tracking_worker.cpp` — recouvre `TrackingWorker` via une homographie de synthèse connue (texture aléatoire déterministe, warp, vérif reprojection < 5 px) + cas « pas de base → pas d'émission ». Cible CMake dédiée avec AUTOMOC + `Qt6::Core` (gardée par `Qt6_FOUND`).

**CI (prio 10, partiel)** : `.github/workflows/ci.yml` — jobs `shell` (`bash -n` + shellcheck informatif) et `compose` (`docker compose config`). Build C++ no-GPU reporté (nécessite `IBOM_ENABLE_ONNX=OFF`, cf §0/§4.2 du doc).

### Fichiers modifiés / créés
- **Créés** : `src/utils/Paths.h`, `src/utils/Paths.cpp`, `tests/test_tracking_worker.cpp`, `.github/workflows/ci.yml`
- **Modifiés** : `CMakeLists.txt`, `cmake/CompilerFlags.cmake`, `src/app/Config.cpp`, `src/app/Application.cpp`, `src/ai/InferenceEngine.cpp`, `src/camera/CameraCapture.cpp`, `tests/CMakeLists.txt`, `docker/compose.yml`, `docker/runtime.Dockerfile`, `docker/entrypoint.sh`, `scripts/run_local_gui.sh`, `.dockerignore`, `.gitignore`, `docs/JETSON_AMELIORATIONS.md`

### Reporté (assumé)
Image runtime réellement minimale (1.3), user non-root + GID numériques (1.5), tags d'images datés (1.8), nettoyage Windows (3.6), CI build C++ no-GPU (4.2). Raisons dans §0 du doc.

### Prochaine étape obligatoire
Sur le Jetson : `git pull` (après `git checkout docker/compose.local.yml`), puis **rebuild + ctest** pour valider. Si la migration du chemin de calibration gêne, copier l'ancien `calibration.yml` vers `~/.local/share/MicroscopeIBOM/`.

---

## Session 2026-06-10 — App lancée sur écran local + audit améliorations + fix #14

### Objectif
1. Acter que l'application **démarre sur l'écran local du Jetson** (rapporté par l'utilisateur)
2. Analyser le projet (Docker, scripts, code C++) et livrer un document de conseils d'amélioration
3. Corriger proprement le doublon `group_add` que l'utilisateur a dû patcher à la main sur le Jetson

### Contexte de départ
- L'utilisateur a lancé la GUI via `scripts/run_local_gui.sh` : **ça fonctionne** ✅
- Il a dû modifier `docker/compose.local.yml` localement : les entrées `video` et `plugdev` étaient en doublon (sémantique de merge compose : les listes sont concaténées entre base et override)
- ⚠️ Constat process : les commits `1316f33` (scripts run_local_gui/cleanup_vnc) et `a022d46` (compose.local.yml) avaient été pushés **sans entrée de journal** — rétro-documentés ici

### Ce qui a été fait
1. **Audit complet** (Docker/compose, scripts, src/, CMake, tests) — résultats consolidés dans **[docs/JETSON_AMELIORATIONS.md](JETSON_AMELIORATIONS.md)** (nouveau), avec tableau de priorisation. Points saillants découverts :
   - 🔴 `runtime.Dockerfile` ne peut pas builder : `COPY build/bin/...` et `COPY docker/entrypoint.sh` sont exclus par `.dockerignore` (build/ et docker/ ignorés)
   - 🔴 Persistance cassée en Docker : `config.json` → `/root/.config`, `calibration.yml` → `/root/.local/share`, cache TRT → `./trt_cache` relatif — aucun ne correspond aux volumes montés par compose → recommandation `IBOM_DATA_DIR`
   - 🔴 `CameraCapture` ne demande pas le FOURCC MJPG → risque 5-10 fps en YUYV sur USB 2.0 quand la caméra sera branchée
   - 🟠 `run_local_gui.sh --force-recreate` tue le container dev (et tout build en cours) à chaque lancement de la GUI
   - 🟠 `QT_QPA_GENERIC_PLUGINS=evdevtouch:...` sous xcb → risque double événements tactiles avec le Minix SF16T
2. **Fix erreur #14** : `compose.local.yml` ne déclare plus que `group_add: [input]` (le reste vient déjà de `compose.yml`) + commentaire expliquant la concaténation des listes au merge. Entrée détaillée dans [JETSON_ERREURS.md](JETSON_ERREURS.md#erreur-14--group_add-dupliques-par-le-merge-composeyml--composelocalyml)

### Fichiers modifiés
- `docs/JETSON_AMELIORATIONS.md` — **nouveau** : rapport d'audit + 11 recommandations priorisées
- `docker/compose.local.yml` — fix doublons group_add (erreur #14)
- `docs/JETSON_ERREURS.md` — entrée #14 (✅ RÉSOLU)
- `docs/JETSON_SESSION_LOG.md` — cette entrée + bloc État actuel

### ⚠️ Action requise sur le Jetson au prochain pull
`compose.local.yml` a été modifié localement sur le Jetson → faire `git checkout docker/compose.local.yml` **avant** `git pull`, le fix du repo remplace le patch manuel.

### Prochaines étapes suggérées (détail dans JETSON_AMELIORATIONS.md §5)
1. Unifier la persistance (`IBOM_DATA_DIR`) + aligner les volumes compose
2. FOURCC MJPG dans `CameraCapture` avant de brancher la caméra microscope
3. Fix `.dockerignore` vs `runtime.Dockerfile`
4. Retirer `--force-recreate` de `run_local_gui.sh`

---

## Session 2026-05-08 — Démarrage migration

### Objectif
Concevoir et lancer la migration de MicroscopeIBOM (projet Windows fonctionnel) vers Jetson AGX Orin 32GB en environnement Docker, sans casser la version Windows.

### Contexte de départ
- Projet C++20 / Qt6 / OpenCV / ONNX Runtime / TensorRT, fonctionnel sur Windows MSVC + vcpkg
- 5 commits Windows récents (camera enumeration thread, mesures, panel inspection, tracking Lowe ratio, doc)
- Décision utilisateur : porter sur Jetson AGX Orin 32GB Seeed reComputer (JP 6.2)

### Décisions prises
| Sujet | Décision | Pourquoi |
|-------|----------|----------|
| Plateforme | Jetson AGX Orin 32GB (Seeed J4012) | Matériel déjà disponible |
| OS | JetPack 6.2 → 7.x quand Seeed compatible | Stabilité actuelle, migration future facile |
| Conteneurisation | Docker via [dusty-nv/jetson-containers](https://github.com/dusty-nv/jetson-containers) | Ne pas polluer L4T, migration JP6→JP7 indolore |
| Niveau de refactor | Medium (phases 0-2 obligatoires, phase 3 optionnelle) | "Ne rien perdre" en perfs |
| Modèle YOLO cible | YOLOv8m FP16 (à confirmer) | Bon compromis vitesse/précision PCB |
| Stratégie d'archivage | Tag + branche `windows-legacy` sur dernier commit Windows | Permettre repli sans pollution |
| Démarrage app | Lancement manuel `docker compose up runtime` | Pas d'autostart systemd pour l'instant |
| Fichiers Windows | Garder en place pendant Phase 1 | Référence pratique, suppression progressive |

### Ce qui a été fait

#### Analyse et planification
- Audit complet du code source pour évaluer la portabilité
- Identification des `#ifdef IBOM_PLATFORM_WINDOWS` (déjà avec branches Linux préexistantes)
- Évaluation des dépendances vcpkg vs apt vs build-from-source
- Analyse du stack JetPack 6.2 (CUDA 12.6, TRT 10.3, cuDNN 8.9)

#### Documents créés
- [docs/JETSON_MIGRATION.md](JETSON_MIGRATION.md) — plan complet (12 sections, ~700 lignes) couvrant :
  contraintes utilisateur, architecture Docker, config matérielle, choix YOLO, plan en phases, layout repo, squelettes Docker, migration JP6→JP7, checklist mise en route, troubleshooting

#### Archivage Windows
- Tag `v0.1.0-windows-final` créé sur `3174dad` (dernier commit Windows réel)
- Branche `windows-legacy` créée sur `3174dad`
- Tag + branche pushés sur origin
- ⚠️ Initialement créés sur `250e327` puis re-pointés sur `3174dad` après rebase pour inclure 5 commits Windows manquants

#### Phase 0 — Docker (livrée)
Commit `40be3fd feat(docker): Phase 0 conteneurisation Jetson AGX Orin`
- [docker/base.Dockerfile](../docker/base.Dockerfile) : multi-stage avec OpenCV CUDA + librealsense2 compilés from source, Qt6 (apt), ZXing-cpp (from source), spdlog/json/libharu (apt)
- [docker/dev.Dockerfile](../docker/dev.Dockerfile) : + gdb, valgrind, vcpkg, ccache, clang-format
- [docker/runtime.Dockerfile](../docker/runtime.Dockerfile) : minimale, binaire stripped + entrypoint
- [docker/compose.yml](../docker/compose.yml) : services base/dev/runtime avec runtime nvidia, network_mode host, devices /dev/video* + /dev/bus/usb + /dev/input + /dev/dri, X11 forwarding
- [docker/run-dev.sh](../docker/run-dev.sh) : wrapper xhost/xauth/lancement interactif
- [docker/entrypoint.sh](../docker/entrypoint.sh) : sanity checks runtime + génération engines TRT au premier lancement
- [docker/README.md](../docker/README.md) : quickstart + troubleshooting
- [scripts/build_jetson.sh](../scripts/build_jetson.sh) : build CMake/Ninja dans le container, ASAN optionnel
- `.gitattributes` : force LF sur scripts (anti-CRLF Windows)
- `.dockerignore` : exclut build/, models/, .git/...

#### Nettoyage repo
- Branche `claude/rework-readme-0TDMk` mergée via cherry-pick (commit `8ae9f2e`)
- Branche remote supprimée
- Pruned les refs locales

#### Journaux créés (cette session)
- [docs/JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) (ce fichier)
- [docs/JETSON_ERREURS.md](JETSON_ERREURS.md)

### Choix techniques notables
- **OpenCV CUDA compilé from source** dans le base.Dockerfile (~90 min) plutôt que copier depuis `dustynv/opencv` → image autonome, indépendante de tags qui peuvent disparaître
- **librealsense2 from source** car pas de paquet apt ARM64 officiel
- **CUDA_ARCH_BIN=8.7** (Ampere — architecture du Jetson AGX Orin)
- **Qt 6.2 LTS via apt** (Ubuntu 22.04) — suffisant pour le projet, upgrade vers 6.6+ uniquement si besoin remonte
- **Multi-stage Docker** : 3 stages (opencv-builder, realsense-builder, final) — réduit la taille finale et permet caching

### À faire prochaine session
1. **Sur le Jetson** :
   ```bash
   git pull
   sudo nvpmodel -m 0 && sudo jetson_clocks
   docker compose -f docker/compose.yml build base   # 90-120 min
   ```
2. Reporter les erreurs éventuelles dans [JETSON_ERREURS.md](JETSON_ERREURS.md)
3. Si `base` build OK → `docker compose build dev` puis `bash docker/run-dev.sh`
4. Dans le container : `bash scripts/build_jetson.sh` (premier build C++ sur Jetson)
5. **Probablement des erreurs de compilation** : c'est le déclencheur de la Phase 1 (nettoyage `#ifdef`)

### Commits poussés cette session
| Hash | Message |
|------|---------|
| `93765fa` | docs: add Jetson AGX Orin migration plan |
| `40be3fd` | feat(docker): Phase 0 conteneurisation Jetson AGX Orin |
| `8ae9f2e` | docs: rework README — structure, précision, lisibilité (cherry-pick) |
| (commit suivant) | docs: add Jetson session and error logs |
| (commit suivant) | docs(claude): mandatory session log discipline |
| `v0.1.0-windows-final` (tag) | Archive Windows |
| `windows-legacy` (branche) | Archive Windows |

### Notes / observations
- Le repo contient encore `build_windows.bat` et `scripts/install_prerequisites.bat` — c'est volontaire (référence pendant Phase 1, à supprimer plus tard).
- Les `#ifdef IBOM_PLATFORM_WINDOWS` dans le code sont laissés en place — la branche `#else` Linux existe déjà donc le code devrait compiler tel quel sur Jetson.
- L'utilisateur travaille **depuis Windows** (cwd `c:\Users\bambo\Desktop\Assistant\Assistant-git`) — les fichiers sont créés ici puis poussés sur GitHub, le Jetson fait `git pull`.

---

<!-- AJOUTER LES NOUVELLES SESSIONS AU-DESSUS DE CETTE LIGNE -->

## Session 2026-05-21 — Premier essai bootstrap Jetson + fix tag image

### Objectif
Lancer le bootstrap (Phase 0) sur le Jetson AGX Orin 32GB Seeed reçu, en MAXN.

### Contexte de départ
- Jetson en MAXN (reboot effectué)
- JetPack 6.2 (L4T R36.4.3) confirmé
- Aucun outil installé (`curl`, `git` absents par défaut)
- Aucun container présent

### Ce qui s'est passé
1. Première tentative one-liner `curl | bash` : échec car `curl` non installé sur le Jetson vierge → script jamais téléchargé, faux background process. Fix : installer `curl wget git ca-certificates` via apt + cloner le repo direct + lancer `bash scripts/bootstrap_jetson.sh` en `nohup` avec `stdin /dev/null`.

2. Bootstrap relancé : étapes 1/8 (vérifs) et 2/8 (MAXN) ✅, échec à 3/8 (test runtime nvidia) :
   ```
   Unable to find image 'dustynv/l4t-jetpack:r36.4.0' locally
   docker: Error response from daemon: pull access denied for dustynv/l4t-jetpack,
   repository does not exist or may require 'docker login'
   ```
   Cause : le repo `dustynv/l4t-jetpack` **n'existe pas** sur Docker Hub (piège anticipé dans `JETSON_ERREURS.md`). Le bon nom est `nvcr.io/nvidia/l4t-jetpack` (image NVIDIA officielle publique).

3. Diagnostic via `docker manifest inspect` sur le Jetson — tags valides identifiés :
   - ✅ `nvcr.io/nvidia/l4t-jetpack:r36.4.0`
   - ✅ `nvcr.io/nvidia/l4t-jetpack:r36.3.0`
   - ❌ `nvcr.io/nvidia/l4t-jetpack:r36.4.3` (n'existe pas — NVIDIA n'a publié que `.0` pour la série 36.4)

4. **Fix** : remplacement de `dustynv/l4t-jetpack` par `nvcr.io/nvidia/l4t-jetpack` partout :
   - [docker/base.Dockerfile](../docker/base.Dockerfile) — 3× `FROM`
   - [scripts/bootstrap_jetson.sh](../scripts/bootstrap_jetson.sh) — commande de test runtime
   - [docker/README.md](../docker/README.md) — exemple `docker run`
   - [docs/JETSON_MIGRATION.md](JETSON_MIGRATION.md) — squelettes du plan (cohérence doc)

   `docker/compose.yml` n'a pas eu besoin de patch (la `L4T_VERSION: r36.4.0` y est valide telle quelle, passée en build arg au Dockerfile).

5. [JETSON_ERREURS.md](JETSON_ERREURS.md) — nouvelle entrée #2 (Docker / image base) ouverte puis fermée en ✅ RÉSOLU dans la même session.

### À faire prochaine étape (côté Jetson après pull)
```bash
cd ~/Assistant-git && git pull
SKIP_PERFMODE=1 bash scripts/bootstrap_jetson.sh > /tmp/bootstrap.out 2>&1 < /dev/null &
disown
tail -f /tmp/bootstrap.out
```
- `SKIP_PERFMODE=1` car MAXN déjà actif
- Docker + nvidia-container-toolkit déjà installés (étapes idempotentes)
- Le test runtime nvidia (3/8) déclenchera maintenant un pull de `nvcr.io/nvidia/l4t-jetpack:r36.4.0` (~quelques minutes)
- Puis étape 6/8 — build base image (~90 min en MAXN)
- Puis étape 7/8 — build dev image (~5-10 min)

### Notes
- Le script bootstrap est bien idempotent — relance sans souci, ne refait pas les étapes déjà OK.
- Si le pull de l'image NVIDIA est lent ou échoue : vérifier la connectivité à `nvcr.io` (pas de NGC login requis pour cette image publique, mais firewall potentiel).

### Suite de la session — fix iptable_raw / network host

6. Nouvelle erreur après le fix précédent : pull d'image OK mais le bridge networking de Docker 29.x panique avec `iptables: Table 'raw' does not exist`. Le kernel Tegra 5.15.148 n'a **pas** le module `iptable_raw` compilé (confirmé `modprobe: FATAL: Module iptable_raw not found`).

7. Diagnostic via les forums NVIDIA Developer + tests locaux :
   - Pas de downgrade simple possible (`docker.io` Jammy n'a que `29.1.3` ou `20.10.12`, pas de version intermédiaire)
   - Recompiler le kernel est hors scope
   - **`--network host` contourne** : test `sudo docker run --rm --network host --runtime nvidia nvcr.io/nvidia/l4t-jetpack:r36.4.0 nvidia-smi` → ✅ retourne `Orin (nvgpu)` + CUDA 12.6
   - Notre stack `compose.yml` utilise déjà `network_mode: host` partout, donc cohérent

8. **Fix appliqué** :
   - [scripts/bootstrap_jetson.sh](../scripts/bootstrap_jetson.sh) : `--network host` ajouté au test runtime nvidia
   - [docker/compose.yml](../docker/compose.yml) : `build.network: host` ajouté aux 3 services (`base`/`dev`/`runtime`) pour que les `RUN apt-get update` internes au `docker build` n'utilisent pas le bridge non plus
   - [docs/JETSON_ERREURS.md](JETSON_ERREURS.md) : nouvelle entrée #3 (Docker / kernel Tegra) ouverte et fermée en ✅ RÉSOLU dans la même session
   - [CLAUDE.md](../CLAUDE.md) : ajout d'un piège #22 dans la section "Pièges critiques" (sticky rule pour les sessions futures)

### À faire prochaine étape (côté Jetson après pull)
```bash
cd ~/Assistant-git && git pull
SKIP_PERFMODE=1 nohup bash scripts/bootstrap_jetson.sh > /tmp/bootstrap.out 2>&1 < /dev/null &
disown
tail -f /tmp/bootstrap.out
```
Étape 3/8 doit maintenant passer ✅. L'image `nvcr.io/nvidia/l4t-jetpack:r36.4.0` est déjà pull localement, instantané. Puis :
- 6/8 build base (~90 min en MAXN, OpenCV CUDA from source) — premier vrai test du `network: host` au build
- 7/8 build dev (~5-10 min)

### Suite — fix paquet Qt6 introuvable

9. Bootstrap relancé avec les 2 fix précédents (image NVIDIA + `--network host`) — passe 3/8 ✅, échoue 6/8 sur `E: Unable to locate package qt6-virtualkeyboard` (piège anticipé en haut de `JETSON_ERREURS.md`).

10. Vérification de **tous** les paquets Qt6 du Dockerfile via `apt-cache show` sur le host Jetson (Jammy arm64) avant de re-patcher : seul `qt6-virtualkeyboard` est introuvable, tous les autres OK. Patch ciblé : remplacé par `qt6-virtualkeyboard-plugin` + `qml6-module-qtquick-virtualkeyboard` (les vrais paquets binaires).

11. [JETSON_ERREURS.md](JETSON_ERREURS.md) entrée #4 ouverte et fermée en ✅ RÉSOLU dans la même session.

### Suite — vcpkg désactivé par défaut

12. Bootstrap relancé après fix Qt6 → étape **6/8 base ✅ RÉUSSI** (OpenCV 4.10 compilé, "=== Stack OK ==="). Échec étape 7/8 `bootstrap-vcpkg.sh` sur ARM64 (binaire `vcpkg-tool` non dispo pour cette cible, fallback compilation from source qui échoue dans le container).

13. **Décision design** plutôt que fix : vcpkg n'a jamais été pertinent sur Jetson (toutes les deps sont en apt natif ARM64). [dev.Dockerfile](../docker/dev.Dockerfile) — `INSTALL_VCPKG` passe de `true` à `false` par défaut. Activable au cas par cas via `--build-arg INSTALL_VCPKG=true`. `VCPKG_ROOT` et `PATH` retirés du `ENV` inconditionnel pour que `build_jetson.sh` fallback proprement.

14. [JETSON_ERREURS.md](JETSON_ERREURS.md) entrée #5 ouverte et fermée en ✅ RÉSOLU dans la même session.

### Suite — devices commentés par défaut + bootstrap Phase 0 ✅

15. Bootstrap COMPLETE après le fix vcpkg : étape 6/8 base + 7/8 dev OK, image `microscope-ibom:dev` créée. **Phase 0 conteneurisation = terminée.**

16. Premier `docker compose up -d dev` échoue : `/dev/video0` mapé en hard requirement, caméra USB pas branchée → Docker refuse. Pour le build C++ on n'en a pas besoin. [docker/compose.yml](../docker/compose.yml) — bloc `devices:` commenté par défaut sur `dev` et `runtime`, à décommenter quand matériel présent.

17. [JETSON_ERREURS.md](JETSON_ERREURS.md) entrée #6 ouverte et fermée en ✅ RÉSOLU dans la même session.

### Suite — Phase 1b (build C++) + Phase 2 (validation runtime UMA)

18. Build C++ premier essai → cascades d'erreurs résolues une à une (13 entrées au total dans JETSON_ERREURS) :
    - **#7** : CMake 3.28 trop récent pour Jammy → baisse à 3.22
    - **#8** : Qt6 introuvable → multiarch dans `CMAKE_PREFIX_PATH`
    - **#9** : Option C (ONNX Runtime from source) — 4 sous-pièges : cmake>=3.26 & <4, clone décomposé+retry, hash Eigen GitLab regen (bug upstream ORT#26707), psutil
    - **#10** : Qt6Gui sans OpenGL → ajout `libgl-dev`, `libegl-dev`, etc.
    - **#11** : `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` cassait FindOpenGL sur Linux → conditionnel WIN32
    - **#12** : Catch2 v3 requis (Jammy n'a que v2.13) → compile from source
    - **#13** : `CV_AUTOSTEP` pas exposé transitivement OpenCV 4.10 Linux → include explicite

19. **Validation Phase 2 UMA runtime** :
    ```
    Test #5: test_unified_allocator ........... Passed 0.13 sec
    100% tests passed, 0 tests failed out of 1
    ```
    + binaire link `libonnxruntime.so.1`, `libcudart.so.12`, `libopencv_*.so.410`, `libQt6*.so.6`
    + smoke test : `GPU: Orin (30698 MB, CUDA 8.7, 14 SMs, TensorCores: yes)` + `CUDA: 12.6, TensorRT: 10.3.0`

20. **Phase 0 + 1a + 1b + 2 OFFICIELLEMENT VALIDÉES SUR JETSON.**

### Bilan de la journée
- **~37 commits poussés** (5 fixes Phase 0 + 7 fixes Phase 1b/2 + commits intermédiaires)
- **~5h de travail effectif** (compute Jetson) + ~1h30 d'attente compile ORT
- **13 entrées d'erreurs documentées** dans JETSON_ERREURS.md, toutes ✅ RÉSOLUES
- Images Docker pérennes : `microscope-ibom:base` (5.91 GB), `microscope-ibom:dev` (6.08 GB)
- Binaire C++ : `build/bin/MicroscopeIBOM` 1.1 MB avec UMA actif

### Commits poussés cette session
| Hash | Message |
|------|---------|
| `ddb4c30` | fix(docker): remplace dustynv/l4t-jetpack par nvcr.io/nvidia/l4t-jetpack |
| `7d16168` | fix(docker): force --network host pour contourner iptable_raw |
| `7145bf0` | fix(docker): qt6-virtualkeyboard → qt6-virtualkeyboard-plugin + qml6 module |
| `847402e` | fix(docker): vcpkg desactive par defaut sur Jetson (opt-in via build-arg) |
| `64a3b13` | fix(docker): devices commentes par defaut (camera/USB opt-in pour le runtime) |
| `cff0567` | fix(cmake): cmake_minimum_required 3.28 → 3.22 pour compat Ubuntu Jammy |
| `d68f975` | fix(build): ajouter le path multiarch au CMAKE_PREFIX_PATH (Qt6 sur apt Jammy) |
| `8fed6e0` | feat(docker): stage onnxruntime-builder — ONNX Runtime ARM64 from source |
| `5f8061b` | fix(docker): clone ONNX Runtime en 2 etapes + retry (anti CANCEL HTTP/2) |
| `68602b9` | fix(docker): pip install cmake>=3.28 dans le stage onnxruntime-builder |
| `3e2a12e` | fix(docker): borner cmake a < 4.x (CMake 4 casse les deps fetched par ORT) |
| `3309895` | fix(docker): patch hash Eigen pour workaround GitLab archive regen (ORT#26707) |
| `34305c2` | fix(docker): cibler cmake/deps.txt au lieu de eigen.cmake pour le sed |
| `19233e2` | fix(docker): ajouter les paquets OpenGL/EGL/Vulkan dev pour Qt6Gui |
| `c9a215d` | fix(cmake): conditionner CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY a Windows |
| `a9149ec` | fix(docker): compile Catch2 v3 from source (apt jammy n'a que v2.13) |
| `3fff57c` | fix(camera): include opencv2/core/cvdef.h pour CV_AUTOSTEP |
| (en cours) | docs: cloture session 2026-05-21 — Phase 0+1+2 validees Jetson, 13 erreurs documentees |

---


### Objectif
Avancer la Phase 2 (mémoire unifiée Tegra) côté code C++ pendant que le test Jetson n'est pas encore lancé. Préparer la fondation du zero-copy pour qu'à la première itération sur le Jetson, la pipeline soit déjà UMA-aware.

### Contexte de départ
- Phase 0 + Phase 1a livrées et pushées sur `main`.
- Pas d'accès matériel Jetson dans cette session — refactor purement code, validable green sur Windows et compile-only sur Jetson tant que pas testé.

### Constat avant refactor
**`FrameBuffer` est du code mort dans le flux principal** : `push()` est appelé à chaque frame ([CameraCapture.cpp:222](../src/camera/CameraCapture.cpp#L222) avant refactor) mais aucun `pop()`/`tryPop()` n'est appelé nulle part dans la base de code. Vérification : `grep -r "frameBuffer()|\.pop\(|\.tryPop\(" src/` → 0 résultats hors définition. La pipeline réelle utilise `FrameRef = shared_ptr<const cv::Mat>` (zero-copy CPU déjà en place depuis avril 2026). Le `frame.copyTo` du `push` était donc une copie pure perte (~190 MB/s à 1080p×3ch×30fps).

### Décisions prises
| Sujet | Décision | Pourquoi |
|-------|----------|----------|
| Que faire de `FrameBuffer` | Supprimer | Dead code, aucun consumer. Si besoin futur de découplage AI, on construira un ring de `FrameRef` (zero-copy même côté ring) plutôt que de cloner des pixels. |
| Approche UMA | Custom `cv::MatAllocator` global, branche `IBOM_USE_UMA_ALLOCATOR` | Plus simple qu'un wrapper `UnifiedFrame` — utilise le hook `cv::Mat::allocator` public. `cap.read(frame)` alloue alors directement dans la mémoire unifiée. |
| Compile flag | `IBOM_ENABLE_UMA=OFF` par défaut, `ON` dans `scripts/build_jetson.sh` | Zéro changement de comportement sur Windows, zéro régression. Sur Jetson : `cudaMallocManaged` actif. |
| Périmètre session | Étapes 2a/b/c (foundation) ; 2d (`InferenceEngine` zero-copy) reportée | Pas de modèle ONNX à tester, pas pertinent sans Jetson tournant. |
| V4L2 DMABUF | Reporté en Phase 2.5 | Gros morceau (remplacer `cv::VideoCapture` par wrapper V4L2), dépend du support caméra. |

### Ce qui a été fait

#### 2a — Suppression `FrameBuffer`
- Supprimé `src/camera/FrameBuffer.h` et `src/camera/FrameBuffer.cpp`
- Retiré `m_frameBuffer`, `frameBuffer()`, le forward-decl et l'`#include` de [src/camera/CameraCapture.h](../src/camera/CameraCapture.h) et [src/camera/CameraCapture.cpp](../src/camera/CameraCapture.cpp)
- Retiré `m_frameBuffer->push(*shared)` du `captureLoop`
- Retiré les deux entrées `FrameBuffer.{h,cpp}` de [CMakeLists.txt](../CMakeLists.txt)
- Pas de test associé à supprimer (FrameBuffer n'avait pas de test)

#### 2b — Nouveau `UnifiedAllocator`
- Créé [src/camera/UnifiedAllocator.h/.cpp](../src/camera/UnifiedAllocator.h)
- `cv::MatAllocator` custom :
  - `allocate(dims, sizes, type, ...)` calcule la taille totale + step (mirror du `StdAllocator` OpenCV) puis alloue via `allocateRaw()`
  - `deallocate()` libère via `deallocateRaw()` qui détecte si le pointeur est managé via `cudaPointerGetAttributes` (gère le cas où UMA flippe off mid-session)
- `allocateRaw()` :
  - Si `IBOM_USE_UMA_ALLOCATOR` défini : `cudaMallocManaged`. Sur échec → bascule définitive vers `std::malloc` (un seul warning, pas de spam)
  - Sinon : `std::malloc` direct
- Singleton via `static UnifiedMatAllocator alloc;` + probe one-shot au premier appel
- `unifiedMemoryAvailable()` : retourne `true` ssi build avec UMA + probe runtime OK

#### 2c — `captureLoop` branché
- [src/camera/CameraCapture.cpp:165](../src/camera/CameraCapture.cpp#L165) : `cv::MatAllocator* alloc = unifiedAllocator();` après ouverture caméra
- Boucle de capture : `frame.allocator = alloc;` AVANT `cap.read(frame)` → la mémoire pixel allouée par OpenCV passe par notre allocator
- Log d'ouverture caméra étendu : `unified memory: yes/no` pour le diagnostic
- Sémantique inchangée côté consumers : `FrameRef = shared_ptr<const cv::Mat>`, fan-out zero-copy via `make_shared<const cv::Mat>(std::move(frame))`

#### CMakeLists
- Nouvelle option `IBOM_ENABLE_UMA` (default OFF)
- Si activé sans `CUDAToolkit_FOUND` → `FATAL_ERROR` explicite
- Define `IBOM_USE_UMA_ALLOCATOR` propagé au target principal sous condition
- Ajout des sources/headers `UnifiedAllocator.{cpp,h}`
- [scripts/build_jetson.sh](../scripts/build_jetson.sh) : `IBOM_ENABLE_UMA=ON` ajouté aux `CMAKE_ARGS`

#### Tests
- Nouveau [tests/test_unified_allocator.cpp](../tests/test_unified_allocator.cpp) — 6 cas Catch2 :
  - Singleton non-null et stable
  - Allocation Mat 720p×3ch via `m.allocator = unifiedAllocator(); m.create(...)`
  - Round-trip pixels (write/read via `at<Vec3b>`)
  - Cycle copy/release (refcount partagé entre `cv::Mat` clones légers, deep copy fonctionne)
  - Compatibilité avec opérations OpenCV (`cv::cvtColor` sur Mat allouée via UMA)
  - Cohérence `unifiedMemoryAvailable()` vs build flag
- [tests/CMakeLists.txt](../tests/CMakeLists.txt) : nouveau target `test_unified_allocator` avec propagation conditionnelle du define `IBOM_USE_UMA_ALLOCATOR` + link `CUDA::cudart` si actif (lecture des `COMPILE_DEFINITIONS` du target principal)

### Pourquoi ce design plutôt qu'un autre

**Pourquoi un `cv::MatAllocator` global plutôt qu'un wrapper `UnifiedFrame`** : OpenCV expose `cv::Mat::allocator` comme membre public, ce qui permet de switcher l'allocator d'une `cv::Mat` SANS toucher à `cv::VideoCapture` (qui appelle en interne `frame.create(rows, cols, type)`). Wrapper aurait forcé une copie après capture (perdant le bénéfice). L'approche allocator est invisible côté caller.

**Pourquoi `cudaMallocManaged` plutôt que `cudaHostAlloc(cudaHostAllocMapped)`** : sur Tegra, les deux donnent un pointeur lisible CPU+GPU sans copie, mais `cudaMallocManaged` est l'API moderne et future-proof (UVM). Sur desktop avec dGPU il y a migration de pages, sur Jetson SoC c'est juste du mapping (pas de migration). L'API `cudaMalloc(... Mapped)` est legacy.

**Pourquoi singleton plutôt que par-instance** : `cv::Mat::allocator` est un pointeur — pas de propriété. Un singleton process-wide simplifie la durée de vie. L'allocator est stateless (pas de pool, pas de state mutable, juste deux fonctions).

### À tester côté Jetson dès que possible
1. `bash scripts/build_jetson.sh` doit produire un binaire avec `UMA: ON` dans le summary CMake
2. À l'ouverture caméra, log doit afficher `unified memory: yes` et `UnifiedAllocator: CUDA Unified Memory active`
3. `ctest` doit passer `test_unified_allocator` (6 cas)
4. `nsys profile` sur une frame de capture → vérifier <1ms en copies host↔device (critère du plan)

### Risques connus / pièges anticipés
- **OpenCV 4.x** : la signature de `cv::MatAllocator::allocate` peut différer entre versions (4.5 vs 4.8 vs 4.12). On a écrit pour 4.x avec `cv::AccessFlag` + `cv::UMatUsageFlags`. Si erreur de compilation côté Jetson (OpenCV 4.8 du Dockerfile), ajuster la signature et logger dans [JETSON_ERREURS.md](JETSON_ERREURS.md).
- **Probe `cudaMallocManaged` peut échouer** si le container Docker est lancé sans `--runtime nvidia` ou si la lib CUDA n'est pas mappée. Dans ce cas l'allocator fallback silencieusement à `std::malloc` (warning unique). Comportement = équivalent à OpenCV par défaut.
- **`cv::Mat::allocator` est un membre public depuis OpenCV 4.0** — si une version future d'OpenCV le rend privé, il faudra utiliser `cv::Mat::setDefaultAllocator` (global, plus invasif).
- **Test `test_unified_allocator`** : sur Windows sans `IBOM_ENABLE_UMA`, le test build sans CUDA (juste opencv_core/imgproc + spdlog), comportement = vérifie le wiring fallback std::malloc.

### À faire prochaine session (Jetson)
1. Sur le Jetson : `git pull && bash scripts/build_jetson.sh`
2. Vérifier `UMA: ON` dans le résumé CMake
3. `cd build && ctest --output-on-failure -R unified_allocator`
4. Lancer le binaire, ouvrir la caméra, vérifier le log `unified memory: yes`
5. Si OK : enchaîner sur Phase 2d (`InferenceEngine` preprocess en-place sur Mat unifiée) — mais d'abord il faudra brancher l'inférence dans `Application` et avoir un `.onnx` réel
6. Si erreurs de compilation OpenCV 4.8 : ajuster signatures `MatAllocator`, logger dans `JETSON_ERREURS.md`

### Commits poussés cette session
| Hash | Message |
|------|---------|
| `f73497f` | feat(camera): Phase 2a/b/c — UnifiedAllocator UMA + suppression FrameBuffer dead code |
| (clôture) | docs: clôture session 2026-05-09 — hash final + état actuel |

## Session 2026-05-08 (suite) — Outillage journaux + Phase 1a

### Objectif
Mettre en place l'infrastructure de journalisation pour permettre la reprise facile, puis attaquer la Phase 1 (portage Linux pur) maintenant que la Phase 0 est livrée.

### Contexte de départ
Phase 0 livrée et pushée. L'utilisateur veut :
- un journal de session pour suivre/reprendre
- un journal des erreurs distinct
- une garantie que les journaux sont à jour même si la session est interrompue (pas de signal "92% contexte" disponible)

### Ce qui a été fait

#### Outillage de journalisation
- Création de [docs/JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) (ce fichier)
- Création de [docs/JETSON_ERREURS.md](JETSON_ERREURS.md)
- Ajout d'une **règle stricte dans [CLAUDE.md](../CLAUDE.md)** : mise à jour obligatoire du journal avant tout `git push` (le journal doit refléter l'état pushé en permanence pour garantir la robustesse aux interruptions)
- Configuration d'un **hook PreCompact** dans `~/.claude/settings.json` (matcher `auto`) : envoie un message rappel "[PRECOMPACT HOOK - JETSON DISCIPLINE]" quand la compression auto du contexte va se déclencher, comme filet de sécurité

#### Synchronisation de la branche `claude/rework-readme-0TDMk`
- Cherry-pick du commit `571a116` (rework README) sur main → nouveau commit `8ae9f2e`
- Suppression de la branche remote
- `git remote prune origin` exécuté

#### Audit du code C++ (avant Phase 1)
Grep complet des `#ifdef _WIN32`, `#ifdef IBOM_PLATFORM_WINDOWS`, `#include <windows.h>` :

| Fichier:ligne | Macro | Branche Windows | Branche Linux |
|---------------|-------|-----------------|---------------|
| `main.cpp:9-19` | `_WIN32` | SEH crash handler | (vide) ⚠️ |
| `main.cpp:23-25` | `_WIN32` | `SetUnhandledExceptionFilter` | (vide) ⚠️ |
| `ai/InferenceEngine.cpp:68` | `_WIN32` | `wstring` UTF-16 | `string` UTF-8 ✅ |
| `camera/CameraCapture.cpp:95` | `IBOM_PLATFORM_WINDOWS` | CAP_MSMF | CAP_V4L2 ✅ |
| `camera/CameraCapture.cpp:114` | idem | MSMF/DSHOW | V4L2 ✅ |
| `app/Config.cpp:16` | idem | `%APPDATA%` | `~/.config/` ✅ |

**Conclusion audit** : le code est **déjà cross-platform** — toutes les branches `#else` Linux sont fonctionnelles. Seul `main.cpp` n'avait pas de handler de crash POSIX (pas bloquant pour le build, juste pour la qualité du diagnostic en cas de crash).

#### Phase 1a — Portage Linux pur (modifications non-régressives)
Modifications minimales pour préparer le build Jetson sans casser le build Windows :

1. **[vcpkg.json](../vcpkg.json)** : conditionner les paquets Windows-only :
   - Feature `msmf` d'opencv4 → `"platform": "windows"`
   - `onnxruntime-gpu` → `"platform": "windows & x64"`
   - Sur Jetson on utilisera TensorRT JetPack système (pas vcpkg)

2. **[src/main.cpp](../src/main.cpp)** : ajout du handler POSIX SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS dans la branche `#else` :
   - `posixCrashHandler()` log via spdlog + backtrace_symbols_fd vers stderr
   - Appelé via `std::signal()` dans `main()`
   - Re-raise du signal pour générer un core dump après log
   - La branche Windows (SEH) reste inchangée

3. **[CMakeLists.txt](../CMakeLists.txt)** : pas modifié — déjà OK avec les branches `if(WIN32) ... elseif(UNIX)`. Les éventuels problèmes find_package() sur Jetson seront traités en Phase 1b après le premier retour de compilation.

#### Pièges anticipés ajoutés dans [JETSON_ERREURS.md](JETSON_ERREURS.md)
- ONNX Runtime non packagé en apt sur Ubuntu 22.04 ARM64 → solutions à explorer (binaire NVIDIA pré-compilé, build from source, ou refactor Phase 2 vers TRT direct)

### Décisions prises
| Sujet | Décision | Pourquoi |
|-------|----------|----------|
| Stratégie journal | Couplage commit↔journal (mise à jour avant push obligatoire) | Robuste à 100% face aux interruptions |
| Hook PreCompact | Filet de sécurité supplémentaire | Compense l'absence de signal "92% contexte" |
| Phase 1 stratégie | Conservative : adapter seulement ce qui est sûrement nécessaire | Le code est déjà cross-platform, pas besoin de big bang |
| `.bat` Windows | Garder en place pendant Phase 1 | Référence pratique, suppression progressive en Phase 1c |
| `#ifdef WIN32` dans C++ | Garder | Branches Linux déjà fonctionnelles, support Windows possible plus tard |
| Handler POSIX dans main.cpp | Ajouté en Phase 1a | Petite valeur ajoutée immédiate, pas de risque |

### Bootstrap script unique (ajout post-Phase 1a)
Création de [scripts/bootstrap_jetson.sh](../scripts/bootstrap_jetson.sh) — script tout-en-un pour partir d'un Jetson vierge JP6.2 et arriver à un container dev prêt :

- Vérifs préalables (Jetson, sudo, internet)
- Mode performance MAXN
- Setup Docker + nvidia-container-toolkit + test runtime nvidia
- Clone du repo (idempotent : git pull si déjà cloné)
- Règles udev RealSense
- Build des images `:base` puis `:dev`
- Recap avec instructions pour la suite

Usage one-liner : `curl -fsSL https://raw.githubusercontent.com/lo26lo/Assistant/main/scripts/bootstrap_jetson.sh | bash`

Override possibles : `REPO_DIR`, `L4T_VERSION`, `SKIP_BUILD`, `SKIP_PERFMODE`.

[docker/README.md](../docker/README.md) mis à jour pour mettre le bootstrap en première option.

### À faire prochaine session
1. **Sur le Jetson** : lancer le bootstrap (one-liner ci-dessus)
2. Reporter les erreurs dans [JETSON_ERREURS.md](JETSON_ERREURS.md)
3. Phase 1b : corrections ciblées selon retours compilateur (probablement ONNX Runtime à régler)

### Commits poussés (cumulé pour le 2026-05-08)
| Hash | Message |
|------|---------|
| `93765fa` | docs: add Jetson AGX Orin migration plan |
| `40be3fd` | feat(docker): Phase 0 conteneurisation Jetson AGX Orin |
| `8ae9f2e` | docs: rework README — structure, précision, lisibilité (cherry-pick) |
| `7ca9c89` | docs: add Jetson session and error logs |
| `34e112e` | docs(claude): mandatory session log discipline |
| `fb76f64` | docs: update session log with logging discipline + complete commit list |
| `78abe3e` | feat(linux): Phase 1a portage — vcpkg conditions + POSIX crash handler |
| `7ce8fea` | feat(scripts): bootstrap_jetson.sh — setup Jetson en une commande |
| (clôture session) | docs: clôture session 2026-05-08 — hashes finaux + état actuel |

### Notes / observations
- Le hook PreCompact est configuré côté global utilisateur (`~/.claude/settings.json`), donc actif aussi en dehors de ce projet — comportement souhaité pour la discipline de journal.
- Si le hook ne se déclenche pas à la première compression auto (caveat watcher), ouvrir `/hooks` pour reload la config ou redémarrer Claude Code.

---

<!-- AJOUTER LES NOUVELLES SESSIONS AU-DESSUS DE CETTE LIGNE NE LE FAIT PAS, ELLE EST AU-DESSUS DE LA SESSION 2026-05-08 D'ORIGINE -->

## Modèle pour nouvelle session

```markdown
## Session YYYY-MM-DD — Titre court

### Objectif
...

### Contexte de départ
...

### Ce qui a été fait
...

### Décisions prises
...

### Erreurs rencontrées
Voir [JETSON_ERREURS.md](JETSON_ERREURS.md) entrées #N à #M

### À faire prochaine session
1. ...

### Commits poussés
| Hash | Message |
|------|---------|

### Notes / observations
...
```
