# Investigation 360° — pistes d'amélioration sur tous les périmètres

> **Date** : 2026-07-06 · **Demande** : « investigation méga profonde d'amélioration qui touche tous les périmètres, pas de changement, un fichier md ».
> **Méthode** : lecture du code (27 700 lignes src+tests, fichiers clés lus en entier : `TrackingWorker`, `CameraCapture`, `Config`, `InferenceEngine`, `ComponentDetector`, `OverlayRenderer`, `BoardLocator`, hot-path de `Application`, CMake, CI, Docker), croisée avec les audits antérieurs ([JETSON_AMELIORATIONS.md](JETSON_AMELIORATIONS.md) 2026-06-10, [IDEES_AMELIORATIONS.md](IDEES_AMELIORATIONS.md) 2026-06-12) pour ne **pas** re-lister ce qui est fait. Tout constat est sourcé `fichier:ligne` (HEAD = `4e9297f`).
> **Aucun code modifié** — ce document est le seul livrable. Matrice de priorisation en §11.

Résumé des plus gros leviers, tous périmètres confondus :

| # | Levier | Périmètre | Impact | Effort |
|---|--------|-----------|--------|--------|
| 1 | **CI C++ réelle sur runner GitHub stock** (technique stub ONNX validée en session 127) | Tests/CI | 🔴 majeur | ~½ session |
| 2 | **Face arrière (Back) inexistante** dans toute la chaîne overlay/alignement | Produit | 🔴 majeur | 1-2 sessions |
| 3 | **Hot-path GUI : copie 1080p + cvtColor par frame sur le thread GUI** | Perf | 🟠 fort | ~½ session |
| 4 | **Letterbox manquant + petits composants invisibles à 640×640** dans le pipeline IA | IA | 🟠 fort (dès qu'un modèle existe) | ~½ session |
| 5 | **Découpage d'`Application.cpp` (4 025 lignes)** en contrôleurs | Dette | 🟠 fort (vélocité) | 2-3 sessions |
| 6 | **Entraîner le modèle** — reste LE déverrouilleur produit (tuto prêt) | IA | 🔴 majeur | hors-code |

---

## 1. Périmètre tracking & alignement (`src/overlay/`)

État : c'est le périmètre le plus abouti (audit 13 findings implémenté, Auto-Align V2, blobs, fix jitter A+B+C). Les pistes restantes :

### 1.1 Auto = 2 fits RANSAC par frame de flow — coût évitable
`TrackingWorker::estimateModel` en mode `Auto` (défaut) fitte **similarité ET homographie** à chaque appel (`TrackingWorker.cpp:263-275`), y compris sur le chemin optical-flow qui tourne **à cadence caméra** (30 Hz). Sur carte plane la similarité gagne quasi toujours (`homogBetter` exige `he < se * 0.7`). Piste : mémoriser la décision quelques dizaines de frames (ré-arbitrer à chaque re-seed ORB seulement) → économise un `findHomography` MAGSAC 30×/s sur l'Orin. Mesurer avant/après au `[track]` timing.

### 1.2 FB-check LK = 2× `calcOpticalFlowPyrLK` CPU par frame
`runOpticalFlow` (`TrackingWorker.cpp:555-562`) fait l'aller **et** le retour à chaque frame. Options si le CPU devient le facteur limitant avec l'IA active : (a) FB-check 1 frame sur N (le glissement LK est lent) ; (b) `cv::cuda::SparsePyrLKOpticalFlow` (OpenCV CUDA est déjà buildé — `IBOM_HAVE_OPENCV_CUDA`) ; (c) VPI (accélérateurs PVA/VIC du Jetson, laisse le GPU à l'inférence). À ne faire que mesure en main.

### 1.3 Seuils de gates en px absolus — dépendants du zoom
`kReanchorMinShiftPx = 12` et `kReanchorConfirmTolPx = 8` (`Application.cpp:1180,1489`), `m_staticThreshPx`, jump gate à 15 % de la diagonale : tous en pixels image. Au microscope fort grossissement, 12 px peuvent représenter 0,05 mm (gate hyper-sensible) ; en vue large 0,5 mm. Piste : exprimer ces seuils **en mm** via `m_currentPixelsPerMm` avec clamps px (même principe que `bootstrapTolMm`).

### 1.4 Rendu des pads : rotation et formes ignorées
`OverlayRenderer::renderBoardSpace` dessine les pads **axis-aligned** (`OverlayRenderer.cpp:115-129`, assumé par commentaire) et réduit RoundRect/Trapezoid/Custom à rect/ellipse. Un composant posé à 45° (fréquent en RF) a des pads faux à l'écran. `Pad` ne porte d'ailleurs pas d'angle dans `IBomData.h` — vérifier ce que l'iBOM fournit (le champ existe dans le JSON pcbdata) et le parser. Effort modéré, gain de fidélité visuelle direct.

### 1.5 Divers courts
- `m_reanchorTickCount` est mort (`Application.h:269`, déclaré, jamais utilisé — résidu du back-off streak retiré en suite 121). À supprimer.
- `ComponentReanchor::Params::useClassPrior` + `classOfComponent` : plomberie présente (`ComponentReanchor.cpp:78-79`) mais **jamais alimentée** par `Application` — dès qu'un modèle à classes existera, câbler le mapping `resources/footprint_classes.json` → gating par classe = moins d'aliasing sur layouts répétitifs (Piste A du plan).
- Le blob detector pourrait exploiter la **depth D405** (hauteur des composants au-dessus du plan carte) comme 3e source de détections/e filtre anti-faux-positifs (sérigraphie = plate). Idée à prototyper hors app d'abord.

---

## 2. Périmètre caméra (`src/camera/`)

### 2.1 Timestamps posés après `cap.read()` — le jitter de décodage entre dans le dt
`captureNs` est pris **après** le retour de `read()` (`CameraCapture.cpp:513`), donc inclut le temps de décode MJPG variable. V4L2 fournit le timestamp buffer du driver (`cv::CAP_PROP_POS_MSEC` ou pipeline GStreamer `do-timestamp`) ; RealSense a `rs2_frame_get_timestamp`. Utiliser le timestamp source rendrait le dt du filtre 1€ encore plus propre (suite logique de F12). Petit effort, gain marginal mais réel.

### 2.2 Pas de reconnexion à chaud
30 échecs consécutifs → arrêt définitif du capture loop (`CameraCapture.cpp:489-494`). Le fallback d'énumération n'existe qu'à l'**ouverture** (`:347-363`). Un débranchement/rebranchement USB en cours de session (fréquent en atelier) exige un redémarrage manuel. Piste : boucle de réouverture avec back-off dans `captureLoop` (ré-énumération incluse), signal `captureError` transformé en état « reconnecting… » dans la status bar.

### 2.3 UMA jamais activée
`IBOM_ENABLE_UMA=OFF` par défaut (`CMakeLists.txt:34`) alors que l'allocateur unifié + son test existent. Sur Orin (mémoire physiquement unifiée), l'activer supprime les copies upload dans le chemin GPU ORB (`detectFeatures` fait `gImg.upload()` par passe — `TrackingWorker.cpp:497-500`). À activer + valider au prochain build, mesurer, puis en faire le défaut Jetson.

### 2.4 Depth sous-exploitée
`m_lastDepthFrame` ne sert qu'à Auto-Align (`Application.cpp:1084`) et au prior d'échelle. Pistes : (a) validation de pose par plan (le plan depth donne l'inclinaison réelle → choix similarité/homographie **informé** au lieu d'heuristique) ; (b) gate de « carte présente » (la récupération LOST pourrait s'abstenir quand la depth dit qu'il n'y a rien à ~10 cm) ; (c) mesures 3D (hauteur composants) dans `Measurement`.

---

## 3. Périmètre rendu & GUI (`src/gui/`, hot-path `Application`)

### 3.1 🔴 Le thread GUI paie ~2 copies plein cadre par frame
Dans le handler `frameReady` (`Application.cpp:1996-2010`) à chaque frame : `cvtColor` BGR→RGB (alloue+copie) puis `qimg.copy()` (2e copie profonde, ~6 Mo à 1080p → ~190 Mo/s à 30 fps), le tout **sur le thread GUI** ; plus `undistort()` quand calibré (backend V4L2). Déjà pointé par IDEES 1.4, toujours ouvert. Pistes par ordre de simplicité :
1. Faire la conversion RGB **dans le thread capture** (le pipeline GStreamer peut même sortir du RGB directement : `videoconvert ! video/x-raw,format=RGB`) → le handler GUI ne fait plus que wrapper.
2. Supprimer le `copy()` : garder le `FrameRef` vivant dans une petite structure {QImage wrappante + shared_ptr} passée à `CameraView` (le zero-copy s'arrête aujourd'hui à la porte de Qt).
3. Undistort : précomputer `initUndistortRectifyMap` une fois et `remap` (aujourd'hui `undistort()` refait le map à chaque appel si non caché — à vérifier dans `CameraCalibration.cpp`), voire `cv::cuda::remap`.

### 3.2 RemoteView : pas d'overlay, pas d'authentification
`pushFrame(display)` envoie l'image caméra **sans l'overlay iBOM** (`Application.cpp:2023-2024` — le warp overlay n'existe que dans `CameraView`). Pour l'usage « montrer la carte à distance », l'overlay est la moitié de la valeur. Piste : réutiliser la composition de `captureView()` (qui sait déjà composer) à cadence réduite. Sécurité : `listen(QHostAddress::Any)` sans aucun token (`RemoteView.cpp:33`) — OK sur LAN d'atelier, mais un PIN/token d'URL est trivial à ajouter et le compose est en `network_mode: host`.

### 3.3 HeatmapRenderer : instancié, jamais nourri, jamais dessiné
`Application.cpp:391` le crée ; aucun `addDefect`/rendu nulle part. Décision à prendre : le câbler au futur SolderInspector (sa raison d'être) ou le sortir de `Application` en attendant — un objet instancié mais mort trouble la lecture de l'archi.

### 3.4 UX de l'état d'alignement
Le nouveau `HELD … waiting for a second concordant estimate` (remède C) n'est visible qu'en log. La status bar/StatsPanel pourraient exposer un mini-état re-anchor (`skipping/held/corrected` + âge de la dernière correction) — diagnostic terrain sans sortir le log. De même le badge tracking ne distingue pas « Locked-par-flow » de « Locked-par-ORB » (utile pour sentir la santé du flow).

### 3.5 i18n absente
`tr()` est partout mais aucun `.ts`/`QTranslator` dans le repo. L'UI est en anglais pour un utilisateur francophone. Générer `fr_FR.ts` + `lrelease` = mécanique. Priorité basse, mais c'est une demi-journée un jour de pluie.

---

## 4. Périmètre IA (`src/ai/`)

### 4.1 🟠 Preprocess sans letterbox — contredit son propre commentaire
`InferenceEngine::preprocess` fait un `cv::resize` **anisotrope** vers 640×640 (`InferenceEngine.cpp:173-197`, le commentaire dit « with letterboxing » mais rien n'est letterboxé) ; le postprocess re-scale en conséquence donc c'est *cohérent*, mais un YOLOv8 entraîné avec letterbox (défaut ultralytics) verra en prod des composants **écrasés 16:9→1:1** qu'il n'a jamais vus à l'entraînement → perte de précision gratuite. Fix : letterbox (pad gris 114) + dé-letterbox des boxes. ~30 lignes. **À faire avant la première éval du modèle**, sinon les métriques terrain seront faussement mauvaises.

### 4.2 🟠 Petits composants : 1080p → 640 = 0201/0402 sous la taille d'un pixel modèle
Un 0402 (1×0,5 mm) à ~7 px/mm fait ~7×3,5 px à 1080p → **2,3×1,2 px** après resize 640. Aucun détecteur ne verra ça. Pistes, par coût croissant : (a) exporter le modèle en **imgsz 1280** (l'Orin le tient en TRT FP16) ; (b) **tuilage** type SAHI pour la passe d'auto-annotation/inspection fine (pas pour le re-anchor, qui se contente des gros) ; (c) inférence sur crop ROI au zoom microscope. À trancher au moment de l'entraînement — impacte le choix d'imgsz du dataset (le Studio produit du 640 ?).

### 4.3 Divers IA
- `detect()` copie le tensor d'entrée CPU à chaque appel — acceptable en événementiel, à revoir si la détection continue 1-2 Hz (roadmap V2 §4) arrive : pré-allouer, et IOBinding pour rester sur GPU.
- `ModelManager` : aucun versioning/checksum des `.onnx` ; le cache d'engines TRT n'est invalidé que par le hash interne d'ORT — documenter « changer de modèle = vider `tensorrt-cache` en cas de doute ».
- `OCREngine` (TODO nu — `OCREngine.cpp:29`) et `SolderInspector` : décider **archivage** (branche/dossier `attic/`) ou roadmap datée. Du code mort avec une API sérieuse fait croire à une feature.
- **Le vrai déverrouilleur reste l'entraînement** (TUTO_DATASETS + `train_on_server.sh` prêts depuis la suite 119) : chaque session de code sur ce périmètre a un ROI inférieur à 2 h de GPU 5070 Ti.

---

## 5. Périmètre iBOM & données (`src/ibom/`)

- **Alignement sauvegardé fragile au chemin** : `SavedAlignment` est keyé sur `ibomFilePath` (`Config.h:99-105`) — déplacer/renommer le HTML perd l'alignement. Keyer sur un **hash du pcbdata** (déjà parsé) + garder le chemin comme hint.
- **Multi-carte** : une seule `SavedAlignment` — l'atelier qui alterne 2-3 cartes re-aligne à chaque switch. En faire une map hash→alignement (même coût).
- Parser : garde anti-zip-bomb en place (`IBomParser.cpp:391-392` ✅ depuis IDEES 1.1). Reste : les `extraFields`/`checkboxes` iBOM sont parsés mais l'app n'expose pas les champs custom en colonnes BOM dynamiques (les `checkboxColumns` sont configurables, les champs custom non).
- `ComponentMap` (grille spatiale) : OK, rien à signaler.

---

## 6. Périmètre produit & features (`src/features/`, `src/export/`)

### 6.1 🔴 La face arrière n'existe pas dans la chaîne AR
`ibom::Layer::Back` n'apparaît **nulle part** dans `Application.cpp` (8 usages hardcodés `Layer::Front` : alignements, re-anchor, bootstrap, dataset) ni dans `OverlayRenderer` (`renderBoardSpace` : `if (comp.layer != Layer::Front) continue;` — `OverlayRenderer.cpp:92`). Le BomPanel a un filtre B/F (`BomPanel.cpp:33`) mais retourner la carte = plus d'overlay, plus d'alignement, plus de dataset. Pour une carte double-face c'est la moitié du travail d'inspection. Chantier : état « face active » global (toggle UI), overlay rendu miroité (x → maxX−x), alignement/re-anchor/dataset paramétrés par la face. C'est le plus gros trou produit identifié par cette investigation.

### 6.2 BarcodeScanner : ZXing est déjà linké, le module dort
`find_package(ZXing REQUIRED)` (`CMakeLists.txt:96`) — la dépendance est payée dans l'image Docker mais `BarcodeScanner` n'est pas instancié. Quick win pick&place : scanner le code de la bobine → sélectionne le groupe de valeurs dans le BOM. ½ session.
- `VoiceControl` : TODO STT (`VoiceControl.cpp:101`) — même décision archivage/roadmap que l'OCR. Whisper.cpp sur Orin est réaliste mais c'est un projet en soi.
- `StencilAlign` : non instancié ; cas d'usage réel au repastillage — à évaluer avec l'utilisateur avant d'investir.

### 6.3 Pick&Place assisté par le tracking
`PickAndPlace` avance au clic. Avec l'overlay AR verrouillé, deux améliorations naturelles : (a) au `markPlaced()`, sauter automatiquement au composant suivant **et centrer/zoomer** la vue dessus (la homographie sait où il est) ; (b) plus tard, confirmation de pose par le détecteur (la classe « présent/absent » suffit).

---

## 7. Périmètre architecture & dette (`src/app/`)

### 7.1 🟠 `Application.cpp` = 4 025 lignes, god-object assumé
`connectControlSignals()` fait ~1 200 lignes à elle seule (`Application.cpp:2265-3472`). Conséquences concrètes : tout ce qui vit là est **intestable** (aucun test ne couvre les gates de re-anchor, le drift gate, la confirmation 2-ticks — la logique la plus délicate du projet) et chaque session paie un coût de navigation. Découpage suggéré, par ordre de valeur :
1. **`AlignmentController`** (autoAlignBoard/componentReanchor/attemptLostRecovery/timer + leurs gates) — c'est LE candidat : logique pure autour de poses, testable avec des résultats synthétiques, changée à chaque session récente.
2. `CameraController` (createCamera/switch/profiles/wireCameraSignals).
3. `AiController` (initializeAI + status).
`Application` resterait le composeur. À faire **par extraction incrémentale** (un contrôleur par session, build Jetson entre chaque), pas en big-bang.

### 7.2 Threading : sain, mais non documenté à un seul endroit
Inventaire vérifié : capture `std::thread` (join OK), TrackingWorker/DatasetCreator `QThread` (quit+wait OK), init IA `std::thread` (join OK — `Application.cpp:100-103`), alignements `QtConcurrent`. Le destructeur est propre. Manque : un §threading dans le README/CLAUDE.md à jour listant **qui écrit quoi** (`m_lastColorFrame` GUI-only, `m_homography` GUI-only, etc.) — la table CLAUDE.md actuelle liste les threads mais pas les règles de propriété, et c'est ce qui protège les futures sessions des data races.

### 7.3 Config
- ~400 lignes de getters/setters boilerplate — vivable ; si un jour ça gratte : macro ou struct publique + `NLOHMANN_DEFINE_TYPE`.
- Un seul versioning partiel (`tracking.defaults_v`). Ajouter un `config_version` global éviterait de re-bricoler une migration par famille de clés à chaque évolution de défauts.

---

## 8. Périmètre tests & CI — le levier le moins cher du repo

### 8.1 🔴 Une CI C++ réelle est possible AUJOURD'HUI sur runner stock
Le blocage historique était « ONNX Runtime absent d'apt » (bandeau de `ci.yml`). Or la session 127 a **prouvé** le contournement : les cibles de test n'utilisent d'onnxruntime que `ai::Detection` (un struct OpenCV pur) — un **stub de 4 lignes** `onnxruntime_cxx_api.h` (`namespace Ort { class Env; class SessionOptions; class Session; class Value; }`) suffit pour compiler et **exécuter** `test_component_reanchor`, `test_blob_detector`, `test_homography`, `test_ibom_parser`, `test_component_matching`, `test_unified_allocator` (CPU) avec les paquets apt (`libopencv-dev`, `catch2`, `nlohmann-json3-dev`, `libspdlog-dev`) ; `qt6-base-dev` débloque en plus `test_tracking_worker` et `test_dataset_creator`. Seul `test_inference` exige le vrai ORT → reste Jetson-only. Concrètement : un job `ci.yml` de ~30 lignes donne **8 cibles sur 9 en CI à chaque push**, sans toucher à la décision « ONNX jamais optionnel » (le stub ne vit que dans le job CI). C'est le meilleur ratio impact/effort de tout ce document — les 3 dernières PR sont parties non compilées.
- Étape 2 : le runner **self-hosted Jetson** (décision 2026-06-10 déjà actée) pour le build complet + ctest CUDA — à formaliser quand voulu.
- Étape 3 : job ASAN sur le sous-ensemble CI-buildable (`IBOM_ENABLE_ASAN` existe déjà).

### 8.2 Trous de couverture, par risque décroissant
1. **Gates d'Application** (drift 12 px, confirmation 2-ticks, épochs anti-stale) — zéro test ; dépend du découpage §7.1 (ou de l'extraction des gates en fonctions libres testables, moins invasif).
2. **BoardLocator** — 451 lignes, zéro test, alors qu'il a un historique d'erreurs (#41/#44) ; des fixtures synthétiques depth+color sont faisables comme pour les blobs.
3. **IBomParser** — testé sur le chemin nominal ; manquent : fixture LZ-String réelle, HTML tronqué (la garde anti-bombe n'est pas exercée), iBOM sans `center`/sans bbox (les régressions #56 ont montré le coût).
4. `Config::load` migrations (defaults_v) — un test JSON v1 → v2.
- Outillage : ni `.clang-format` ni `.clang-tidy` dans le repo — en ajouter fige le style pour toutes les sessions (et `clang-format --dry-run` en CI est gratuit).

---

## 9. Périmètre Docker / Jetson / infra

- **`runtime.Dockerfile` jamais buildé** (relevé 1.3 de l'audit de juin, toujours vrai) — le jour où l'atelier veut un déploiement propre (kiosque), ce sera le chemin ; le valider une fois évite de le découvrir cassé sous pression. Lié : user non-root (1.5, reporté) et autostart systemd/kiosque.
- **Registry d'images** : la base (~90 min de build) n'existe que localement sur le Jetson. La pousser sur ghcr.io (même manuellement) = un `docker pull` au lieu de 90 min en cas de réinstallation/SSD mort. Quick win de résilience.
- **Perf Orin non pilotée** : rien ne documente/force `nvpmodel`/`jetson_clocks` (l'entrypoint vérifie le GPU mais pas le power mode). Un mode 30 W vs MAXN change les budgets de tous les threads mesurés. À documenter dans docker/README + option entrypoint.
- **tegrastats → StatsPanel** : `GpuUtils` lit la mémoire GPU mais pas la charge GPU/EMC réelle Tegra (`tegrastats`/sysfs). Utile le jour où flow GPU + TRT + rendu se battront pour le GPU.
- Veille JP7 : le fix `iptable_raw` (ERREUR #3) lèvera la contrainte `--network host` — à re-tester à la sortie.

---

## 10. Périmètre docs & process de session

- **`JETSON_SESSION_LOG.md` ne scale plus** : ~128 suites dans un fichier lu **en premier à chaque session**. Le coût de contexte croît linéairement. Piste : archiver les suites > 3 mois dans `JETSON_SESSION_LOG_ARCHIVE_*.md` en gardant en tête un « État actuel » consolidé de ~1 page + les 10 dernières suites. Même logique pour `JETSON_ERREURS.md` (déjà 56+ entrées) : séparer l'index (à jamais) des entrées résolues anciennes.
- **Dette de validation non agrégée** : les « ⚠️ non compilé ici / à valider Jetson » sont éparpillés dans les suites. Un bloc unique « À VALIDER AU PROCHAIN BUILD » en tête de journal, purgé à chaque build réel, éviterait d'en perdre (il y en a actuellement au moins : fusion #21, suites 126-127, UMA si activée).
- 24 fichiers dans `docs/` : l'assainissement de juin (bandeaux OBSOLÈTE) a bien vieilli ; il manque juste un `docs/INDEX.md` d'une page qui dit quoi lire pour quel besoin.
- CLAUDE.md : la liste « pièges » mélange Windows-legacy et Jetson — la scinder en deux sections rendrait le préambule de session plus court.

---

## 11. Matrice de priorisation

**Impact** : produit/robustesse/vélocité. **Effort** : ½s = demi-session.

| Priorité | Item | § | Impact | Effort | Dépend de |
|----------|------|---|--------|--------|-----------|
| **P0** | ✅ **Fait (suite 129)** — CI C++ stub ONNX (9/9 cibles à chaque push, `scripts/ci_unit_tests.sh`) | 8.1 | Majeur (filet permanent) | ½s | rien |
| **P0** | Entraîner `component_detector.onnx` | 4.3 | Majeur (débloque détection, re-anchor stable, P&P assisté) | hors-code | tuto prêt |
| **P1** | ✅ **Fait (suite 129)** — Letterbox (`src/ai/Letterbox.h` + test) ; décision imgsz/tuilage reste ouverte (l'app s'adapte à l'imgsz du .onnx) | 4.1-4.2 | Fort | ½s | avant l'éval du modèle |
| **P1** | ✅ **Fait (suite 132)** — Hot-path GUI : wrap zéro-copie `Format_BGR888` (cvtColor ET deep-copy supprimés ; l'undistort remap était déjà caché) | 3.1 | Fort (CPU Orin) | ½-1s | mesure Jetson à confirmer |
| **P1** | ✅ **Fait (suite 131)** — Face arrière (Back) bout en bout (overlay miroité, alignements, re-anchor, dataset, UI ; convention : H = PCB brut → image, det < 0 en vue arrière) | 6.1 | Majeur produit | 1-2s | validation Jetson |
| **P1** | Consolidation journal + bloc « à valider » | 10 | Fort (vélocité sessions) | ½s | rien |
| **P2** | 🟡 **Étape 1 faite (suite 133)** — gates du re-anchor silencieux extraits en `overlay::ReanchorGate` unit-testé (8 cas) ; l'extraction complète `AlignmentController` reste à faire | 7.1/8.2 | Fort (testabilité) | 1s/contrôleur | CI P0 en place |
| **P2** | Reconnexion caméra à chaud | 2.2 | Moyen (confort atelier) | ½s | rien |
| **P2** | RemoteView : overlay composité + token | 3.2 | Moyen | ½s | rien |
| **P2** | ✅ **Fait (suites 130+134)** — Tests BoardLocator + fixtures parser (full-parse, cross-ref BOM, régressions #56 & bbox rotée) | 8.2 | Moyen | 1s | CI P0 |
| **P2** | UMA ON + mesure | 2.3 | Moyen (perf) | ¼s + build | build Jetson |
| **P3** | Seuils gates en mm ; décision Auto mémorisée ; timestamps driver | 1.1/1.3/2.1 | Faible-moyen | ¼s chacun | rien |
| **P3** | BarcodeScanner (ZXing déjà payé) ; P&P auto-advance | 6.2/6.3 | Moyen (niche) | ½s | rien |
| **P3** | Pads rotation/formes ; i18n fr ; ghcr registry ; nvpmodel doc | 1.4/3.5/9 | Faible | ½s chacun | rien |
| **P3** | Archiver OCR/Voice/Stencil ou roadmap datée ; retirer Heatmap d'Application ; `m_reanchorTickCount` | 4.3/3.3/1.5 | Hygiène | ¼s | décision utilisateur |

**Lecture recommandée du trio de tête** : P0-CI d'abord (une demi-session, puis chaque autre chantier de cette liste est protégé), l'entraînement du modèle en parallèle (c'est du temps GPU, pas du temps de session), puis 4.1 pour que le modèle fraîchement entraîné soit évalué à sa vraie valeur.
