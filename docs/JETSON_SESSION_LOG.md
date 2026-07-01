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

## État actuel — au 2026-07-01 (Lot C : overlay warpé espace-carte + transformée inline)

> **2026-07-01 (suite 111)** : **implémentation du lot C** de [LIVE_TRACKING_ANALYSE_2026-07.md](LIVE_TRACKING_ANALYSE_2026-07.md) (suite de « fais b et c »). **Changement d'architecture du rendu overlay** — le plus gros morceau des 3 lots :
> - **F11 — overlay warpé** : `OverlayRenderer` réécrit autour de **`renderBoardSpace()`** — l'overlay (pads/silk/labels de la face avant) est rendu **une seule fois en espace carte** (buffer ARGB ~2048 px côté long, marge 2 mm, **antialiasing réactivé** — ce rendu ne se refait que sur changement de sélection/placed/toggles/couleurs/projet, jamais par frame). `CameraView` gagne un **canal board overlay** (`setBoardOverlayImage` + `setBoardOverlayTransform`) : à chaque paint, le buffer est **warpé par un QTransform projectif** composé `bufferToPcb⁻¹ × H` (helper `OverlayRenderer::toQTransform` — attention **transposition** : cv = vecteurs colonnes, QTransform = vecteurs lignes). Stocké en **QPixmap** pour que le moteur GL cache la texture entre les paints (la QOpenGLWidget re-uploadait une QImage à chaque frame sinon). Conséquences : **suppression du cap 40 ms** (`m_lastOverlayRenderMs`) et de la **signature homographie** (`m_ovSigHomography`/`m_ovSigSize`/`m_ovSigFab` supprimés d'`Application.h`) — la pose ne coûte plus que 9 doubles/frame ; l'overlay est **toujours collé à la pose la plus fraîche au moment du paint** (adresse aussi une partie de F12) ; le clear ERREUR #46 préservé (image nulle poussée une fois quand l'homographie devient invalide). **Le canal overlay plein-cadre reste** pour le feedback de picking 4 points, qui en devient l'**unique propriétaire** (nouveau `m_pickOverlayShown` pour le relâcher une fois ; il n'est plus supprimé quand un overlay iBOM valide existe → pendant un re-alignement les deux sont visibles). `captureView()` (screenshots) compose le même warp. **Changement d'UX assumé** : les **labels vivent en espace carte** → ils **grossissent avec le zoom** (style AR, taille ~0,6 mm normal / 0,9 mm sélection) au lieu d'une taille écran fixe ; les largeurs de silk aussi. Fallback bbox : si `boardBBox` est dégénéré (iBOM sans contours), union des bbox composants.
> - **F13 — transformée inline** : `Homography` cache les 9 coefficients (et l'inverse) en `double[9]` à chaque `compute`/`setMatrix`/`load` ; `pcbToImage`/`imageToPcb`/vecteur/`transformRect` appliquent le produit 3×3 + division perspective **inline** — l'ancien chemin construisait 2 `std::vector` + un wrap Mat + `cv::perspectiveTransform` **par point** (l'overlay/minimap/scale en font des milliers par frame). Comportement conservé (`!m_valid` → point inchangé) ; rayon dégénéré w≈0 → point inchangé (cv renvoyait ±inf). **Test de parité** vs `cv::perspectiveTransform` ajouté (`test_homography.cpp`, matrice réellement projective, aller + retour + overload vecteur).
>
> Fichiers : `src/overlay/OverlayRenderer.{h,cpp}` (réécrits), `src/overlay/Homography.{h,cpp}`, `src/gui/CameraView.{h,cpp}`, `src/app/Application.{h,cpp}` (bloc overlay frameReady réécrit, membres signature réduits, +`m_boardBufferToPcb`/`m_pickOverlayShown`), `tests/test_homography.cpp`, docs. ⚠️ Non compilé ici. **Risques identifiés à valider au build Jetson (validation visuelle obligatoire)** : (1) le moteur de paint **GL** de la QOpenGLWidget doit warper correctement un QTransform projectif (raster OK par construction ; si artefacts GL → repli possible : rendre via QImage/raster) ; (2) **netteté au fort zoom microscope** — buffer 2048 px : si l'overlay paraît flou, monter `kMaxDim` à 4096 dans `OverlayRenderer.cpp` ; (3) alignement exact overlay↔image sous zoom/pan/fullscreen (comparer au comportement d'avant) ; (4) opacité overlay + screenshots + reset alignment + picking. **À mesurer** : log `[overlay] board buffer re-rendered … in X ms` (rare) et fluidité générale — le GUI thread ne re-tesselle plus jamais par frame. **Restent** : lots D (F8+F10+F6) et E (F12 timestamps).

## État actuel — au 2026-07-01 (Lot B : robustesse flow + anti-saut)

> **2026-07-01 (suite 110)** : **implémentation du lot B** de [LIVE_TRACKING_ANALYSE_2026-07.md](LIVE_TRACKING_ANALYSE_2026-07.md) (demande utilisateur : « fais b et c » — le c suit en suite 111). Tout dans `src/overlay/TrackingWorker.{h,cpp}` + un test :
> - **F3 — pruning des outliers du set optical-flow** : `estimateModel` gagne un paramètre de sortie optionnel `cv::Mat* inlierMask` (masque Nx1 uchar du fit retenu, y compris en mode Auto via un helper `deliver`). `runOpticalFlow` filtre désormais le set de landmarks par ce masque **à chaque frame** — un point LK qui avait glissé sur une feature voisine ne survit plus jusqu'au re-seed (30 frames) à voter contre le bon modèle. **Re-seed anticipé** : si le set pruné tombe sous 2×`minMatchCount`, `m_flowFramesSinceDetect = m_flowRedetectInterval` → la frame suivante part sur l'ORB au lieu d'agoniser sur un set famélique.
> - **F9 — contrôle forward-backward (MedianFlow)** : après le LK aller (`m_prevGray→fullGray`), re-track **retour** des points arrivés ; rejet si ‖retour − départ‖ > 0,5 px. Attrape le mode de défaillance classique du LK (glissement silencieux sub-pixel sur une feature voisine), invisible aux sorties status/err. Coût : 2e appel `calcOpticalFlowPyrLK` sur ≤200 pts (~qq ms, budget Orin OK).
> - **F4 — gates anti-aberration dans `emitHomography`** (sortie unique des 3 chemins) : (a) le **gate qualité** rejette aussi `reprojErr > ransacThreshold` (fit « mou ») ; (b) **sanité géométrique** via nouveau helper `projectedArea2()` (shoelace signé du polygone board projeté) — coins finis exigés (NaN passe tous les comparateurs numériques sinon !), aire non effondrée (|2A| ≥ 1 px²), et **pas d'inversion d'orientation** vs la pose de référence (`m_lastEmittedH` sinon `m_baseHomography` — comparaison relative, donc compatible avec un alignement miroir face arrière) ; (c) **gate de saut** : déplacement des coins > 15 % de la diagonale frame (`m_frameDiag`, posé dans `processFrame`) ⇒ la pose est mise en attente (`m_pendingJumpH`) et **2 estimées consécutives concordantes** (tolérance 7,5 % de la diagonale — laisse passer le mouvement réel continu entre deux ticks) sont exigées ; sur confirmation, `m_cornerFilters.clear()` → l'overlay **snap** au lieu de traîner à travers le saut. `m_pendingJumpH` reseté dans `resetReference`. Le premier EMIT après reset (m_lastEmittedH vide → disp -1) n'est pas affecté — la récupération du lot A reste immédiate.
> - **Test** : « holds a huge single-frame jump until confirmed » — grande carte texturée (masque couvrant), baseline émise, puis warp +200 px (20 % de la diagonale) : 1er tick HELD (émissions inchangées), 2e tick confirmé (pose à <5 px du vrai décalage).
>
> Fichiers : `src/overlay/TrackingWorker.{h,cpp}`, `tests/test_tracking_worker.cpp`, docs (analyse F3/F9/F4 ✅, ce journal). ⚠️ Non compilé ici. **À valider au build Jetson** : (1) `ctest` (4 tests worker) ; (2) en verbose, plus aucun EMIT aberrant isolé (chercher `HELD jump`/`HELD insane` dans le log lors de passages de main/glare) ; (3) le flow reste fluide (le FB-check ne doit pas assécher le set en usage normal — surveiller la fréquence des re-seeds ORB) ; (4) mouvement brusque réel : l'overlay suit avec ~1 tick de latence de confirmation (~100 ms), puis snap net.

## État actuel — au 2026-07-01 (Lot A de l'audit live tracking implémenté)

> **2026-07-01 (suite 109)** : **implémentation du lot A** de [LIVE_TRACKING_ANALYSE_2026-07.md](LIVE_TRACKING_ANALYSE_2026-07.md) (choix utilisateur : « lot A »). Quatre fixes livrés :
> - **F1 — défauts Config = fast path** : `Config.h` passe les défauts à `trackingIntervalMs=100`, `trackingModel=0` (Auto), `trackingOpticalFlow=true` (CLAHE/GPU inchangés). **Migration douce** dans `Config::load()` : nouvelle clé `tracking.defaults_v` (persistée par `save()`, défaut membre = 2) ; les fichiers pré-v2 (clé absente → 1) voient **uniquement les valeurs encore égales aux anciens défauts** upgradées (200→100 ms, model 3→0, flow false→true) — les réglages custom délibérés survivent ; idempotent tant que save() n'a pas persisté defaults_v. Le spin Settings (50-2000 ms) et le combo Model (Auto en index 0) couvrent déjà les nouvelles valeurs, zéro changement UI.
> - **F2/ERREUR #51 — fallback masque escaladant** (✅ RÉSOLU dans le journal d'erreurs) : `m_lostFrames` devient le compteur d'échecs **des deux modes** via le nouveau helper `noteDetectMiss()` (++ , log debug, Lost à ≥4). `processFrame` escalade le masque : ×1.6 (<3 échecs) → ×2.5 (3-5) → **plein cadre** (≥6) ; `buildBoardMask` prend désormais `marginScale` en paramètre. Garde-fous : l'escalade exige `m_hasReference` (la capture de référence reste masquée — sinon retour de l'ERREUR #35) ; et `m_lastHomography` + `seedFlowLandmarks` ne sont plus alimentés que par un fit **sain** (`inliers ≥ minMatchCount`) — une estimée dégénérée ne peut plus déplacer le masque. Reset du compteur sur tout succès (référence, flow — ajouté dans `runOpticalFlow` —, incrémental/hybride déjà en place). Les deux `if (++m_lostFrames >= 4) setState(Lost)` incrémentaux remplacés par `noteDetectMiss()` (comportement identique).
> - **F5 — badge Locked en mode référence** : `processReference` fait `setState(State::Locked)` sur fit sain (il ne posait jamais d'état → badge UI bloqué sur « LOST » en ORB pur). Commentaires du header (`State`, `trackingStateChanged`) mis à jour (plus « incremental-only »).
> - **F7/ERREUR #52 — scale IBomPads** (✅ RÉSOLU) : paire de pads de référence **cachée par projet** (`m_scaleRefProject` tag d'identité jamais déréférencé + positions copiées par valeur `m_scaleRefA/B`, `m_scaleRefDistMm`) recalculée en un passage O(n) — extrêmes des deux diagonales (x+y, x−y), paire la plus éloignée des deux candidates (≥ diagonale/√2). Le call site live (`homographyUpdated`) est **throttlé à 5 Hz** (`m_lastScaleUpdateMs`) ; les appels événementiels (alignement, re-anchor) restent directs.
> - **Test de régression ajouté** (`tests/test_tracking_worker.cpp`) : « TrackingWorker re-acquires after the board leaves the masked area » — patch texturé sur fond uni, polygone board posé, référence capturée, puis translation +400 px hors masque ×1.6 : sans le fix, aucune émission (deadlock #51) ; avec, ré-acquisition ≤ 12 frames, pose recouvrée à <5 px et badge Locked (couvre F2+F5).
> - Docs : CLAUDE.md (table des défauts : interval 100, + lignes trackingModel/trackingOpticalFlow), erreurs #51/#52 passées ✅ RÉSOLU (avec « à valider au build Jetson »), findings F1/F2/F5/F7 et lot A marqués ✅ dans l'analyse.
>
> Fichiers : `src/app/Config.{h,cpp}`, `src/overlay/TrackingWorker.{h,cpp}`, `src/app/Application.{h,cpp}`, `tests/test_tracking_worker.cpp`, `CLAUDE.md`, `docs/{LIVE_TRACKING_ANALYSE_2026-07,JETSON_ERREURS,JETSON_SESSION_LOG}.md. ⚠️ **Non compilé ici** (pas de toolchain). **À valider au prochain build Jetson** : (1) build + `ctest` (dont le nouveau test masque) ; (2) au premier lancement, log « Migrating tracking defaults v1→2 » puis Settings → Tracking affiche 100 ms/Auto/Optical-flow coché ; (3) déplacer brusquement la carte hors champ du masque → l'overlay ré-accroche seul en <1 s (log `[track] detect/match miss #N`) ; (4) badge « Tracking: locked » visible en ORB pur (flow décoché) ; (5) scale px/mm stable avec `scaleMethod=IBomPads`. **Restent** : lots B (F3+F9+F4), C (F13, F11 selon re-capture), D (F8+F10+F6), E (F12).

## État actuel — au 2026-07-01 (Audit complet du live tracking étape par étape)

> **2026-07-01 (suite 108)** : **analyse** (pas de code) — demande utilisateur : « analyse le live tracking étape par étape pour faire mieux ». Audit statique complet du pipeline tel qu'écrit (post PR #20 + suites 92-101), livré dans **`docs/LIVE_TRACKING_ANALYSE_2026-07.md`** : schéma du flux en 12 étapes avec verdict par étape (références fichier:ligne), **13 findings** priorisés P0→P3, plan d'action en 5 lots (A-E) validables une session Jetson chacun, protocole de validation. **Constat d'orientation** : le cœur d'estimation est excellent quand le flow est actif (logs suite 100 : inliers 171-200, reproj <0.06 px) — les gains restants sont dans (1) **les défauts de config** (out-of-the-box = flow OFF, modèle Homography 8 DOF, 200 ms = le chemin legacy « ça suit mal », F1), (2) **la robustesse aux cas limites**, (3) **l'architecture d'affichage**. Points saillants : **F2/ERREUR #51** 🔴 masque board sans récupération (perte définitive possible — `m_lastHomography` jamais remis à jour après échec, aucun compteur d'échec en mode référence) ; **F7/ERREUR #52** 🔴 `updateDynamicScale` IBomPads bogué (`bestDist` reset par composant → padB faux) + scanné à ~30 Hz ; **F3** le set de landmarks flow garde les outliers RANSAC jusqu'au re-seed ; **F4** aucune garde anti-saut sur l'estimée émise (le 1€ suit les sauts par design) ; **F5** l'état ne passe jamais Locked en mode référence (badge menteur) ; **F8** re-seed flow à compteur fixe = micro-saut ~1 Hz ; **F10** delta incrémental toujours 8 DOF (ignore `m_model`) → dérive microscope accélérée ; **F11** proposition structurelle : overlay rendu une fois en espace carte puis **warpé par QTransform projectif** à chaque paint (supprime la re-tessellation par frame, AA réactivable, cap 40 ms inutile) ; **F12** pas de timestamps de capture → overlay 1-2 frames derrière la vidéo, non mesurable/compensable ; **F13** `Homography::pcbToImage` = 2 vecteurs + `perspectiveTransform` **par point**, appelé des milliers de fois par rendu. Ordre recommandé : lot A (F1+F5+F2+F7) → B (F3+F9+F4) → C (F13, F11 si la re-capture le justifie) → D (F8+F10+F6) → E (F12). Étape 0 du protocole : **re-capturer un log verbose** pour mesurer `[overlay] in X.Xms` post-fixes suite 100 (toujours non validés). Fichiers : `docs/LIVE_TRACKING_ANALYSE_2026-07.md` (nouveau), `docs/JETSON_ERREURS.md` (+#51 +#52), `docs/LIVE_TRACKING_PLAN.md` (bandeau de renvoi), `docs/JETSON_SESSION_LOG.md`. **Aucun code touché** — prochaine étape : choix utilisateur du lot à implémenter (A recommandé).

## État actuel — au 2026-06-22 (Script d'entraînement serveur Ubuntu / RTX 5070 Ti)

> **2026-06-22 (suite 107)** : **outil** — l'utilisateur entraînera sur un **serveur Ubuntu avec RTX 5070 Ti** (Blackwell sm_120). `scripts/train_on_server.sh` : variante de `build_reanchor_model.sh` pour Linux natif GPU. Différences clés : (1) **venv isolé** (`.venv-train`) pour ne rien casser ; (2) **PyTorch installé depuis l'index `cu128`** (`https://download.pytorch.org/whl/cu128`) — **obligatoire pour Blackwell RTX 50xx**, un torch trop ancien donne « no kernel image is available » ; (3) **contrôle GPU** (`torch.cuda.is_available()` + nom + capability) avant d'entraîner ; (4) `--device` configurable, batch 32 par défaut (16 Go). Puis fetch → remap `--presence` → train (yolov8n/100ep) → export `models/component_detector.onnx`. Clé API non commitée (placeholder + garde-fou). `bash -n` OK, `chmod +x`. Note : `.venv-train/` et `datasets/`, `runs/` ne devraient pas être commités (à ajouter au .gitignore si besoin — pas fait ici). Fichier : `scripts/train_on_server.sh`.

## État actuel — au 2026-06-22 (Script tout-en-un Roboflow runnable dans Docker)

> **2026-06-22 (suite 106)** : **outil** demandé par l'utilisateur (« un script pour tout ce qui est à faire avec Roboflow, runnable dans le Docker, je remplacerai par la bonne clé »). `scripts/build_reanchor_model.sh` : enchaîne en une commande install deps → fetch → remap `--presence` → train (yolov8n/80ep) → export `models/component_detector.onnx`. **Clé API NON commitée** (placeholder `COLLE_TA_CLE_ICI` + support `$ROBOFLOW_API_KEY` ; garde-fou qui refuse de tourner si clé absente). Bloc CONFIG éditable en tête (URL dataset, epochs, batch, `RUN_TRAINING=0` pour télécharger+préparer seulement et entraîner ailleurs). Gestion gracieuse de l'échec pip/torch sur Jetson (avertit + pointe Colab/PC GPU, le dataset préparé restant réutilisable). `set -euo pipefail`, `bash -n` OK, `chmod +x`. `docs/TUTO_MODELE_REANCRAGE.md` : ajout section « ⚡ Méthode express » en tête pointant ce script. ⚠️ Note : l'entraînement torch dans le conteneur app (ORT/TensorRT) n'est pas garanti installable sur Jetson aarch64 — d'où l'option `RUN_TRAINING=0`. Fichiers : `scripts/build_reanchor_model.sh`, `docs/TUTO_MODELE_REANCRAGE.md`.

## État actuel — au 2026-06-22 (Tuto débutant pour produire le modèle)

> **2026-06-22 (suite 105)** : **doc** — `docs/TUTO_MODELE_REANCRAGE.md` à la demande utilisateur (« tuto comme si j'étais nul »). Guide pas-à-pas 7 étapes pour produire `component_detector.onnx` (Piste B présence) et l'activer : compte Roboflow + clé API, install `pip` (ultralytics/roboflow/pyyaml), fetch → remap `--presence` → train (yolov8n, 80 epochs) → export ONNX → copie Jetson + test (menu Dev « Component re-anchor now », Auto re-anchor, logs `[comp-reanchor]`). Cible d'entraînement = **PC Windows RTX 5070** (le Jetson ne fait que lancer l'app). Inclut : section Piste A condensée, **annexe Google Colab** (zéro install navigateur, avec caveat dépôt privé → upload manuel des scripts), et un tableau « problèmes courants » (pip PATH, accès dataset, CUDA OOM → `--batch`, .txt oublié, 1er lancement long = engine TRT, pose de départ requise). Fichier : `docs/TUTO_MODELE_REANCRAGE.md`.

## État actuel — au 2026-06-22 (Piste A : fusion datasets + workflow complet documenté)

> **2026-06-22 (suite 104)** : **Piste A — outillage data**. `scripts/merge_datasets.py` : fusionne plusieurs sources YOLO (datasets publics remappés **+** sessions `DatasetCreator` `$IBOM_DATA_DIR/dataset/session_*/`) en un dataset unique `out/{images,labels}/{train,val}` + `data.yaml` (14 classes par défaut, `--presence` ou `--names` possibles). Split **déterministe** (hash MD5 du nom → reproductible), tag de source pour éviter les collisions de noms, détection des paires image/label par convention `labels/`↔`images/`. **Testé fonctionnellement ici** (fixture synthétique : 2 sources de styles différents → train=7/val=1, data.yaml 14 classes correct). `docs/AI_MODEL_DATASETS_PLAN.md` complété : §5 (merge ✅), nouveau §7 « Workflow concret » avec les **commandes pas-à-pas B et A** (fetch→remap→train→export pour B ; capture DatasetCreator→remap→merge→train→export pour A). **Décision actée dans le doc : B puis A** ; le **code des deux est en place**, il ne reste que l'exécution data/entraînement (machine GPU + nos captures). **Class prior** (`useClassPrior` dans `ComponentReanchor`) laissé en **raffinement futur** : sûr uniquement si l'ordre des classes du modèle == `footprint_classes.json`, ce qui exige d'exposer les noms de classes du modèle au runtime — non fait pour éviter un mismatch d'index silencieux non testable ici. Le matching spatial seul marche déjà pour modèle présence (B) **et** 14 classes (A). Fichiers : `scripts/merge_datasets.py`, `docs/AI_MODEL_DATASETS_PLAN.md`.

## État actuel — au 2026-06-22 (Piste B implémentée : re-ancrage composant IA + pipeline d'entraînement)

> **2026-06-22 (suite 103)** : **implémentation Piste B** (re-ancrage robuste au niveau composant, cf. `docs/AI_MODEL_DATASETS_PLAN.md`). Deux moitiés livrées. **(1) Pipeline d'entraînement (Python, à lancer sur machine GPU, PAS le Jetson)** : `scripts/fetch_roboflow_dataset.py` (télécharge un dataset Universe au format YOLOv8 via clé API + parse l'URL workspace/project), `scripts/remap_classes.py` (mode `--presence` = toutes classes→1 `component` pour B ; mode `--map fichier.yaml` = source→nos 14 classes pour A ; réécrit labels + data.yaml en place), `scripts/class_mapping.example.yaml` (gabarit de mapping), `scripts/train_yolo.py` (wrapper Ultralytics, imgsz **figé 640** car `InferenceEngine` attend 640×640). Les 4 compilent (`py_compile` OK). Export ONNX existant inchangé. **(2) Câblage app C++** : nouveau `src/overlay/ComponentReanchor.{h,cpp}` — `estimate(detections, project, currentPose, layer, …)` : projette chaque composant iBOM via la pose courante (prior), matche chaque détection au composant prédit le plus proche dans un rayon (`maxMatchDistPx=60`, assignation gloutonne mutuellement exclusive), `cv::findHomography(RANSAC)`, valide via inliers (`minInliers=8`) + médiane reproj (`≤8px`). **Classe optionnelle** (`useClassPrior`, off par défaut → marche avec un modèle « présence » 1 classe). `Application::componentReanchor(bool silent)` (miroir d'`autoAlignBoard`) : clone frame + snapshot pose, lance `detector->detect()` + `estimate()` sur `QtConcurrent`, applique via watcher (gate de dérive 12px en silencieux, `setMatrix`, reset référence tracking, maj px/mm + minimap). **Le timer périodique de re-anchor préfère désormais `componentReanchor()` si un détecteur est chargé**, sinon retombe sur `autoAlignBoard()` (BoardLocator géométrique). **Pourquoi c'est le bon fix D405** : le re-ancrage composant marche **quand la carte remplit le cadre** — exactement là où BoardLocator échoue. Déclenchement manuel ajouté : menu **Dev → « Component re-anchor now »** (signal `MainWindow::componentReanchorRequested`). Fichiers : `scripts/*.py`, `scripts/class_mapping.example.yaml`, `src/overlay/ComponentReanchor.{h,cpp}`, `CMakeLists.txt` (+2 entrées), `src/app/Application.{h,cpp}`, `src/gui/MainWindow.{h,cpp}`, `docs/AI_MODEL_DATASETS_PLAN.md`. ⚠️ **Non compilé ici** (pas de toolchain) — à builder sur Jetson. ⚠️ **Inerte tant qu'aucun `.onnx` n'est dans `models/`** : produire un modèle « présence » via le pipeline ci-dessus pour activer B. **Reste pour A** : `merge_datasets.py` + fine-tune sur sortie `DatasetCreator`.

## État actuel — au 2026-06-22 (Plan datasets publics + modèle de détection)

> **2026-06-22 (suite 102)** : **plan** (pas de code) suite au partage de 3 datasets PCB Roboflow par l'utilisateur (`roboflow-100/printed-circuit-board`, `pcb-component-detection-v2`, `marco-filippozzi/smd-component-detection`). Nouveau doc **`docs/AI_MODEL_DATASETS_PLAN.md`**. Points clés : (1) **2 objectifs distincts** — A = `ComponentDetector` fin (overlay, exige nos 14 classes par boîtier), B = **re-ancrage auto robuste** (n'a besoin que des **positions** → tolérant aux classes grossières). (2) **Problème central = granularité** : nos CI sont séparés par boîtier (`ic_soic/qfp/qfn/bga`, dérivés des footprints KiCad), les datasets publics ne le font pas → remap obligatoire, finesse perdue (OK pour B, pénalisant pour A). (3) **Verdict datasets** : SMD (Filippozzi) = meilleur, component-detection-v2 = repli, RF100 = à écarter. (4) **Stratégie 2 pistes** : B rapide (YOLOv8n « présence » public → ONNX → re-ancrage boîtes↔iBOM, débloque D405 gros plan) ; A fond (pré-entraîne public → fine-tune sur sortie `DatasetCreator` = nos cartes/caméra). (5) **Licences à vérifier** avant diffusion de poids (Roboflow renvoie 403 au fetch auto). (6) Outils à écrire : `fetch_roboflow_dataset.py`, `remap_classes.py`, `merge_datasets.py`, + stratégie re-ancrage composant côté app. **En attente** : choix utilisateur piste A ou B avant d'écrire le code. Fichier : `docs/AI_MODEL_DATASETS_PLAN.md`.

## État actuel — au 2026-06-22 (Grisage Incremental hors microscope + clarif Auto re-anchor)

> **2026-06-22 (suite 101)** : **UX** suite à question utilisateur (« ça devrait être grisé sur D405 »). La case **« Incremental frame→frame tracking »** (mode microscope) et son spin **« Re-anchor drift »** sont désormais **grisés quand le backend ≠ V4L2** (RealSense) — le worker les ignorait déjà sur RealSense (gate V4L2), mais la case active était trompeuse. Implémenté dans `SettingsDialog::updateCameraResolutionUI()` (appelé au chargement + à chaque changement de backend) : `setEnabled(!isRS)` + tooltip « Microscope-only (V4L2). Ignored on the RealSense backend. ». Fichier : `src/gui/SettingsDialog.cpp`. **Auto re-anchor : volontairement PAS grisé par backend** — il n'est pas spécifique D405, il marche en champ large (carte ne remplissant pas le cadre + marge de fond) ; il est inutile seulement quand la carte remplit le cadre (cas D405 gros plan de l'utilisateur) ou au microscope zoomé. Honnêteté communiquée : dans les 2 setups réels de l'utilisateur, le re-ancrage géométrique ne sert quasi à rien → le vrai re-ancrage robuste = composant-level IA (attend un modèle). ⚠️ Non compilé ici.

## État actuel — au 2026-06-22 (Diagnostic logs : overlay render ~1s sature le GUI → AA off + throttle)

> **2026-06-22 (suite 100)** : **diagnostic du « pas rapide » via les logs verbose de l'utilisateur** (D405, build à jour). **Le tracking est excellent** (`[track] EMIT inliers=171-200 reproj<0.06px`). **MAIS le pipeline d'affichage tourne à ~1.5 fps** : `[overlay] re-rendered` espacés de **0.6–1.3 s**, et les EMITs arrivent en **rafales suivies de ~1.3 s de silence totale** (ni track ni overlay loggés). Pattern sans ambiguïté : **un seul rendu overlay synchrone prend ~0.6–1.3 s et bloque le thread GUI** → les frames caméra s'empilent puis se libèrent en rafale. **Pourquoi maintenant** : la suite 95 (optical flow dé-throttlé) fait que l'homographie se met à jour **chaque frame (~30 Hz)** au lieu de tous les 200 ms → le **rendu overlay coûteux** (carte D405 en gros plan **remplit le cadre** → les 133 composants + tous les pads dessinés, **antialiasés**) tourne ~30×/s sur le GUI et l'étouffe. Le FULL STATE DUMP montre aussi `incremental=true` (config — mais gaté V4L2 donc inactif sur RealSense) et `reanchor enabled` qui spamme (carte plein cadre, confirmé suite 98). **Fixes (à confirmer par re-capture)** : (1) **antialiasing OFF** sur les formes pads/silk dans `OverlayRenderer::render` (AA software de milliers de polygones = le coût ; text-AA gardé pour les labels) ; (2) **cap du re-render overlay à ~25 fps** (`m_lastOverlayRenderMs`, gate 40 ms) — le tracker rapide ne sature plus le GUI, la vidéo caméra reste à 30 fps ; (3) **timing loggé** : `[overlay] re-rendered … in X.Xms` → la prochaine capture **confirmera** le coût réel et l'effet du fix. Fichiers : `src/app/Application.{h,cpp}`, `src/overlay/OverlayRenderer.cpp`. ⚠️ Non compilé ici. **Conseils config immédiats à l'utilisateur** : décocher **Auto re-anchor** (carte plein cadre = inutile) et **Incremental** (mode microscope, pas D405). **À valider** : re-capturer un log verbose → vérifier `[overlay] … in Xms` (attendu nettement < 40 ms après AA off) et la disparition des trous de 1 s ; si encore lent, passer le rendu overlay hors-thread proprement (worker tracking).

## État actuel — au 2026-06-22 (Système de logs debug exhaustifs, activable depuis le menu Dev)

> **2026-06-22 (suite 99)** : **système de logging debug exhaustif, togglable depuis le menu Dev** (demande utilisateur : pouvoir activer des logs « pour tout ce qu'on peut modifier » et avoir accès à TOUT, idéalement via un connecteur MCP qui lance le programme + lit les logs). **Constat infra** : le `file_sink` est **déjà au niveau `trace`** (`Logger.cpp`), gaté seulement par le niveau du logger → un simple `Logger::setLevel(trace)` fait que le fichier capture **tout**. Implémenté :
> - **Menu Dev** : (1) **« Verbose debug logging »** (checkable) → `Logger::setLevel(trace/info)` + persiste `features.verbose_logging` + dump complet à l'activation ; (2) **« Dump full state to log »** → snapshot à la demande ; (3) **« Copy log file path »** → met `Logger::logFilePath()` dans le presse-papier (pour trouver/lire le fichier).
> - **`Application::logFullState()`** : dump **info** (toujours enregistré) de TOUS les réglages + état runtime — camera (backend/index/rés/fps/hwdecode/depth), scale (méthode/mult/px-mm), calibration (rms), tracking (interval/orbKp/minMatch/lowe/ransac/downscale/model/1€/clahe/opticalFlow/gpu/incremental/hybrid), re-anchor (enabled/interval/score/streak), overlay (toggles/opacités/couleurs), AI (enabled/model/conf/ready), state (ibom/components/homographyValid/selectedRef/placed/depth/cloud), log (verbose/file). Appelé au toggle, au « Dump », et à chaque Apply Settings si verbose.
> - **Logs debug ciblés** (visibles seulement verbose ON) : `emitHomography` logge **chaque décision** — `EMIT` / `HELD low-quality (inliers<min)` / `HELD static-scene (cornerDisp<seuil)` → **explique directement « ça suit mal »** ; le handler `homographyUpdated` logge inliers/reproj/scale (métriques avant ignorées) ; overlay re-render logge taille/toggles/sel/placed/comps.
> - **Startup** : applique `verbose_logging` persisté + coche le menu.
>
> Fichiers : `src/app/Config.{h,cpp}` (verbose_logging), `src/gui/MainWindow.{h,cpp}` (menu Dev + signals + copy-path), `src/app/Application.{h,cpp}` (handlers + `logFullState` + debug ciblés), `src/overlay/TrackingWorker.cpp` (`emitHomography` debug). ⚠️ Non compilé/testé ici. **Workflow visé** : Dev → cocher « Verbose debug logging » → reproduire le problème → « Copy log file path » → me partager le fichier (ou un connecteur MCP shell/fs côté Jetson le lit). **À valider** : (1) build OK ; (2) le menu Dev montre les 3 entrées ; (3) cocher verbose → le fichier log se remplit du stream `[track] EMIT/HELD…`, `[overlay]…`, etc. ; (4) « Dump full state » écrit le bloc complet. **Note MCP** : je ne peux pas lancer l'app ni lire le fichier depuis ce conteneur (pas d'accès Jetson) — il faudrait un connecteur MCP (shell/fs) branché sur le Jetson ; le fichier log est l'artefact à me transmettre en attendant.

## État actuel — au 2026-06-22 (Build OK ! + fix spam BoardLocator + back-off re-anchor)

> **2026-06-22 (suite 98)** : ✅ **LE BUILD PASSE ET L'APP TOURNE SUR LE JETSON** (screenshots utilisateur, D405, FPS 30, tracking locked, Optical-flow coché, case « Auto re-anchor » bien présente dans Settings → Tracking). Les 7 commits empilés (fix dup/link, revert overlay sync, OverlayRenderer recyclé, optical-flow dé-throttlé, plan B + case Settings) **compilent et runnent**. **Problème observé** : avec « Auto re-anchor » activé sur la D405 en gros plan, l'**Event Log est spammé d'un WARN toutes les 3 s** : « BoardLocator: Depth: detected region (368351 px²) is larger… ». Cause : la carte **remplit ~90 % du cadre** (368351 px² ≈ 90 % de 848×480) → la segmentation depth attrape quasi tout le cadre, `validateSize` rejette (région ≫ taille carte attendue à 4.4 px/mm), `BoardLocator::locate` retourne not-found et **loggait ce résultat normal en WARN**. **Fixes** : (1) `BoardLocator::locate` : log not-found passé en **`debug`** (un not-found est un résultat normal ; le chemin Auto-Align interactif logge déjà son propre message utilisateur) → plus de spam. (2) **Back-off** du re-ancrage silencieux : `m_reanchorFailStreak` (++ sur not-found, reset sur found) + skip de ticks proportionnel (`tickCount % (streak+1)`) → quand BoardLocator n'y arrive pas (carte plein cadre), les tentatives s'espacent au lieu de hammerer Canny+orientations toutes les 3 s ; reprend à pleine cadence dès que la carte est trouvable. Fichiers : `src/overlay/BoardLocator.cpp`, `src/app/Application.{h,cpp}`. ⚠️ **Limite confirmée en pratique** : le re-ancrage géométrique **ne marche pas** quand la carte remplit le cadre (pas de séparation carte/fond) — typique de la D405 en gros plan → **conseiller de décocher Auto re-anchor** dans ce cas ; le gain pertinent ici reste l'**optical flow**. ⚠️ Non recompilé depuis ces 2 fixes. **À valider** : Event Log plus spammé ; vérifier aussi si l'**overlay s'affiche/est aligné** (screenshot ambigu — Auto-Align échoue ici, alignement manuel requis).

## État actuel — au 2026-06-20 (Case Settings pour le re-ancrage périodique)

> **2026-06-20 (suite 97)** : **UI Settings pour le plan B**. Ajout dans `SettingsDialog` onglet Tracking : case **« Auto re-anchor on PCB outline (corrects drift) »** (`m_autoReanchorEnabled` → `reanchor_enabled`) + spin **« Auto re-anchor interval »** (0.5–30 s, `m_autoReanchorInterval` → `reanchor_interval_s`), avec tooltips (dont la limite « bords du PCB visibles, pas le microscope zoomé »). Noms distincts du `m_reanchorDrift` préexistant (qui est le seuil de dérive du **mode incrémental microscope**, feature différente). Load/save câblés ; à l'Apply, `MainWindow::settingsChanged` → `Application::updateReanchorTimer()` (déjà connecté suite 96) démarre/arrête le timer et applique l'intervalle. `reanchor_min_score` reste réglable via config.json (pas de spin, défaut 0.5). Fichiers : `src/gui/SettingsDialog.{h,cpp}`. ⚠️ Non compilé/testé ici. **À valider** : Settings → Tracking montre la case + l'intervalle ; cocher → re-ancrage auto actif sans éditer le JSON.

## État actuel — au 2026-06-20 (Plan B : re-ancrage géométrique périodique BoardLocator, sans modèle)

> **2026-06-20 (suite 96)** : **implémentation du « plan B » — re-ancrage géométrique périodique** (vers un tracking hybride « Yollo-like » sans modèle IA). Idée : pendant le live tracking, re-localiser périodiquement le contour du PCB (`BoardLocator`, pur-CV, déjà utilisé par Auto-Align) et **recaler l'homographie** pour annuler la dérive accumulée — l'optical flow + ORB assurent la fluidité entre deux re-ancrages.
> - **`autoAlignBoard(bool silent=false)`** : nouveau mode **silent** (re-ancrage auto). En silent : **aucune UI** (pas de status/dialogue), et on n'applique le résultat **que** si (a) `score ≥ reanchorMinScore` ET (b) la pose `BoardLocator` **diverge de > 12 px** de la pose courante (sinon le tracking est sain → on ne touche à rien, pas de stutter inutile via `resetReference`). Le mode interactif (clic Auto-Align) est **inchangé** (popups + warning low-confidence conservés via `if (!silent)`).
> - **Déclencheur** : `QTimer m_reanchorTimer` (intervalle = `reanchor_interval_s`, défaut 3 s) ; à chaque tick, si `reanchor_enabled && m_liveMode && m_ibomProject && !m_autoAligning` → `autoAlignBoard(silent=true)`. `updateReanchorTimer()` start/stop + intervalle depuis Config ; appelé à l'init et sur `settingsChanged`. La frame est déjà cachée chaque frame (`m_lastColorFrame`).
> - **Config** : `tracking.reanchor_enabled` (false), `reanchor_interval_s` (3.0), `reanchor_min_score` (0.5) — load/save JSON.
> - **Fusion** : BoardLocator = vérité terrain périodique qui reset la dérive ; entre deux, optical flow (suite 95) + ORB. Le re-ancrage fait `resetReference` (nouvelle base) + recalcule l'échelle.
>
> Fichiers : `src/app/Application.{h,cpp}`, `src/app/Config.{h,cpp}`. ⚠️ **Pas d'UI dans SettingsDialog** pour l'instant → **activer via `config.json`** : `"tracking": { "reanchor_enabled": true }` (ou j'ajoute une case Settings en suite). ⚠️ **Limite honnête** : BoardLocator a besoin de **voir les bords du PCB** → OK en champ large (D405), **PAS au microscope fort-grossissement** (bords hors champ). Précision = niveau contour de carte (plus grossier que le tracking composant-level — pour le 0201 il faudrait le re-ancrage IA composant-level, qui attend un modèle entraîné). ⚠️ Non compilé/testé ici. **À valider** : (1) build OK ; (2) activer `reanchor_enabled` + iBOM chargé + Auto-Align/tracking actif → quand le tracking dérive (>12 px), l'overlay se recale tout seul toutes les ~3 s, sans popup ; (3) quand le tracking est sain, **aucune perturbation** (shift < 12 px → skip) ; (4) si l'overlay « saute » vers une pose pire, monter `reanchor_min_score` (BoardLocator pas assez fiable sur cette carte/fond).

## État actuel — au 2026-06-20 (Optical flow dé-throttlé : tourne à pleine cadence, ORB seul throttlé)

> **2026-06-20 (suite 95)** : **fix « pas rapide » (option (b))** — le throttle `trackingIntervalMs` (200 ms ≈ 5 Hz par défaut) était **en tête de `processFrame`**, donc il bridait **aussi le chemin optical-flow** (Phase 3) → l'optical flow, censé tourner à la cadence caméra (c'est sa raison d'être), tournait à 5 Hz comme l'ORB. **Fix** : déplacer le throttle **après** le fast-path optical-flow. Désormais : le **LK optical flow tourne à chaque frame** (≈30 Hz, suivi fluide), et seul le **re-détection ORB** (cher : detect+match+RANSAC) reste paced par `m_intervalMs`. Le cycle de re-seed est intact (`seedFlowLandmarks` remet `m_flowFramesSinceDetect=0`, `runOpticalFlow` l'incrémente ; à `m_flowRedetectInterval` frames le fast-path est sauté → ORB re-détecte (throttle déjà écoulé car 30 frames ≫ 200 ms) → re-seed). L'émission passe toujours par `emitHomography` (gate scène statique Phase 1 + lissage 1€ Phase 2 conservés). Effet de bord bénin : en mode **ORB pur** (flow off), `cvtColor` (+CLAHE si activé) tourne maintenant à chaque frame au lieu de 5 Hz — négligeable (CLAHE off par défaut). Fichier : `src/overlay/TrackingWorker.cpp`.
>
> ⚠️ **L'optical flow est OFF par défaut** (`m_opticalFlow=false`, choix conservateur PR #20). **Pour bénéficier de ce fix, l'utilisateur DOIT cocher « Optical-flow » dans Settings → Tracking** (réglage live, pas de rebuild). Sans ça, on reste en ORB pur throttlé à `m_intervalMs` (baisser l'intervalle vers ~40 ms = option (a), non faite). ⚠️ Non compilé/testé ici. **À valider** : (1) build OK ; (2) activer Optical-flow → suivi nettement plus fluide (overlay suit à ~30 Hz entre les re-détections ORB) ; (3) surveiller la dérive entre re-seeds (baisser `m_flowRedetectInterval` si l'overlay glisse avant le refresh ORB) ; (4) recovery après perte (flow échoue → ORB reprend immédiatement, non throttlé sur frame d'échec).

## État actuel — au 2026-06-20 (Revert palier 3 : overlay re-rendu SYNCHRONE — fix flicker live tracking)

> **2026-06-20 (suite 94)** : **retour utilisateur sur le live tracking : « flicker, ça suit mal et pas rapide »**. Diagnostic du **flicker** : c'était le **palier 3 (rendu overlay asynchrone QtConcurrent)** — l'overlay et l'image caméra étaient sur **deux timelines différentes** (l'image se met à jour chaque frame dans `frameReady`, l'overlay seulement quand le worker async avait fini, plusieurs frames plus tard). En mouvement, l'overlay « flottait » derrière l'image et se mettait à jour par à-coups → flicker + impression de mauvais suivi. C'était le risque explicitement signalé en suite 92. **Fix : revert vers un rendu SYNCHRONE** — l'overlay est re-rendu sur le thread GUI dans `frameReady` et poussé **avec** la frame (`setOverlayImage(OverlayRenderer::render(in))`), donc toujours verrouillé sur l'image. **On garde les vrais gains** : rendu **sur changement** (signature — static/posé saute le rendu), **frustum culling** (dans `OverlayRenderer::render`, garde le coût/frame bas même quand le live tracking re-rend chaque frame), et la **centralisation** dans `OverlayRenderer::render` (suite 93). Supprimés : `m_overlayWatcher`, `m_overlayInFlight`, le bloc de création du watcher + son handler `finished`, et le dispatch `QtConcurrent::run`. Fichiers : `src/app/Application.{h,cpp}`.
>
> ⚠️ **« suit mal et pas rapide » n'est PAS entièrement réglé par ce revert** : le revert tue le flicker, mais la **lenteur du suivi** vient surtout du **throttle `trackingIntervalMs` = 200 ms par défaut → ~5 mises à jour/s** (même si la caméra tourne à 30 fps). C'est le levier n°1 restant (cf. mon avis sur l'analyse ChatGPT de l'utilisateur : la plupart de ses « phases » — filtre temporel, 1€/EMA, optical flow KLT, hybride — sont **déjà** dans PR #20 Phases 1-3 ; le point neuf et actionnable est l'intervalle). **À proposer/faire** : descendre `trackingIntervalMs` vers ~30-50 ms (et noter que le `config.json` existant de l'utilisateur peut surcharger le défaut → à changer dans Settings → Tracking, ou supprimer la clé). ⚠️ Non compilé/testé ici. **À valider** : (1) build OK ; (2) overlay **collé à l'image** en live tracking, **plus de flicker** ; (3) si encore lent, baisser l'intervalle de tracking.

## État actuel — au 2026-06-20 (OverlayRenderer tranché : recyclé comme moteur de rendu unique)

> **2026-06-20 (suite 93)** : **résolution du sort d'`OverlayRenderer`** (demande utilisateur : le câbler comme chemin unique plutôt que le laisser en code mort). Contexte : le palier 3 (suite 92) avait déjà centralisé le dessin de l'overlay dans une fonction libre `renderOverlayImage` (anonyme, en tête de `Application.cpp`). J'ai donc **fait d'`OverlayRenderer` le foyer de ce rendu** : `src/overlay/OverlayRenderer.{h,cpp}` réécrits → une struct `overlay::OverlayInputs` (le snapshot) + une **méthode statique sans état `static QImage OverlayRenderer::render(const OverlayInputs&)`** (travail pixel pur, frustum culling, polices 1×/rendu, **aucun accès widget/QObject** → appelable depuis le worker QtConcurrent). L'ancienne API morte est **supprimée** : `render(cv::Mat)` (jamais appelée), `drawComponentOutline/Pads/Label/Pin1/SelectionEmphasis/BoardOutline`, `setHomography/setIBomData/setHighlightedRefs/setShowPads/setShowLabels/setComponentState/setActiveLayer`, `stateColor`, `matToQImage`, et **tout l'état** (`m_homography/m_project/m_componentStates/m_highlightedRefs/...`). La classe **n'est plus un `QObject`** (plus de signaux). Côté `Application` : suppression du membre `m_overlayRenderer` + son instanciation + **les ~24 sites qui poussaient son état à chaque événement** (tous des no-ops : ils alimentaient le `render()` jamais appelé) ; le dispatch du palier 3 appelle désormais `overlay::OverlayRenderer::render(in)`. La fonction libre `renderOverlayImage` est retirée d'`Application.cpp` (déplacée dans `OverlayRenderer`). Résultat : **un seul chemin de rendu overlay**, dans son module naturel (hors du `Application.cpp` de 3000 lignes), et **zéro code mort**.
>
> Fichiers : `src/overlay/OverlayRenderer.{h,cpp}` (réécrits), `src/app/Application.{h,cpp}` (helper déplacé, membre + instanciation + ~24 sites supprimés, dispatch recâblé). ⚠️ **Non compilé/testé ici**. La suppression des 24 sites = des **no-ops** (ils alimentaient un renderer jamais lu), donc aucun changement de comportement runtime attendu — seulement du code en moins. **À valider au prochain build Jetson** : (1) build OK (vérifier qu'aucun site `m_overlayRenderer` résiduel ni include manquant — `grep` local confirme 0) ; (2) overlay **strictement identique** au comportement de la suite 92 (le rendu est le même code, juste relocalisé). Risque principal : un include manquant dans le nouvel `OverlayRenderer.cpp` (QString/QPolygonF/IBomData) — ajoutés explicitement.

## État actuel — au 2026-06-20 (Optim overlay paliers 1+2+3 : rendu hors thread GUI, sur PR #20)

> **2026-06-20 (suite 92)** : **optimisation d'efficience du chemin d'affichage — paliers 1+2+3 combinés, construits directement sur la branche PR #20** (décision utilisateur : rebaser le perf sur PR #20 puis faire le palier 3 ; en pratique j'ai ré-appliqué 1+2 par-dessus le chemin overlay réécrit par PR #20 et ajouté le palier 3, plutôt qu'un `git rebase` des vieux commits — équivalent mais plus propre). **Constat** : le handler `frameReady` (`Application.cpp`) tourne sur le **thread GUI** et rebâtissait l'overlay vectoriel iBOM plein cadre (alloc `QImage` ARGB + `QPainter` sur **tous** les composants) **à chaque frame, même sans changement, sans culling**.
> - **Palier 3 (cœur) — rendu hors thread GUI** : le rendu lourd part sur un worker **`QtConcurrent`** (même pattern que `autoAlignBoard`/`CalibrationMonitorDialog`, déjà éprouvé ici, `Qt6::Concurrent` déjà lié). Nouvelle fonction libre `renderOverlayImage(OverlayRenderInputs)` (anonyme, en tête de fichier) = travail pixel pur, **aucun accès widget/Application** → thread-safe. Le thread GUI ne fait que **snapshotter des copies par valeur** (`shared_ptr<const IBomProject>` immuable + `Homography` valeur + couleurs/toggles/selected/placed) et dispatcher ; un `QFutureWatcher<QImage>` (membre `m_overlayWatcher`, créé dans `createSubsystems`) redélivre l'image finie **sur le thread GUI** → `CameraView::setOverlayImage`. Garde `m_overlayInFlight` : pas de nouveau dispatch tant que le précédent n'est pas fini (anti-empilement, convergence vers le dernier état). Résultats périmés (homographie devenue invalide pendant le rendu) **droppés** dans le handler `finished`.
> - **Palier 1 — re-rendu sur changement** : signature des entrées (homographie via `cv::norm NORM_INF`, sélection, hash du set placed, toggles pads/silk/fab, couleurs, taille, identité `IBomProject`) comparée chaque frame ; dispatch **seulement** si ça change. Carte posée/alignée (microscope) : ~30 rendus/s → ~0. Polices de label construites **une fois par rendu** (étaient une fois **par composant**).
> - **Palier 2 — frustum culling** : dans `renderOverlayImage`, on saute les composants dont le bbox projeté n'intersecte pas le cadre (décisif au fort grossissement).
> - **Bonus** : `InferenceEngine` threads intra-op ONNX = `hardware_concurrency` borné à 8 (au lieu de 4 codé en dur).
> - **`OverlayRenderer` (code mort, `render()` jamais appelé)** : laissé en place — suppression de ~15 sites à l'aveugle = risque pur, gain nul. À nettoyer dans un passage dédié.
>
> Fichiers : `src/app/Application.{h,cpp}` (helper + dispatch + watcher + signature/culling + membres), `src/ai/InferenceEngine.cpp`. Le `#46 fix` PR #20 (clear overlay quand homographie invalide) est **préservé** dans la branche `else`. ⚠️ **Non compilé/testé ici** (pas de toolchain). Le palier 3 touche le **threading Qt** (classe de bugs que seul un build Jetson attrape), mais via `QtConcurrent` (pas de metatype custom, pas de QThread manuel → risque moindre qu'un worker bespoke). **À valider au prochain build Jetson** : (1) build + `ctest` OK ; (2) overlay iBOM **identique visuellement** (pads/silk/labels, couleurs, sélection, placed) ; (3) carte posée alignée → l'overlay ne se re-rend plus chaque frame (CPU GUI plus bas) ; (4) **live tracking** → l'overlay suit sans bloquer la boucle d'événements (vérifier qu'il n'y a pas de lag perceptible overlay↔image, et que `m_overlayInFlight` ne fige pas l'overlay si un rendu reste bloqué) ; (5) Reset Alignment efface bien l'overlay ; (6) toggles pads/silk/fab + changement de couleurs en Settings → l'overlay se met à jour. **Si l'overlay clignote/disparaît en live tracking**, soupçonner le drop « stale » trop agressif ou un `m_overlayInFlight` jamais reset (vérifier que `finished` est bien émis).

## État actuel — au 2026-06-20 (2e erreur build PR #20 — link test : modules OpenCV manquants)

> **2026-06-20 (suite 91)** : 2e itération du build Jetson de PR #20. **Bonne nouvelle : le binaire `MicroscopeIBOM` compile ET linke** (`[63/64]`) — le chemin GPU Phase 3 compile sans erreur. Seul **le test `test_tracking_worker` échoue au link** : `cv::calcOpticalFlowPyrLK` (module `opencv_video`) et `cv::cuda::ORB::create` (module `opencv_cudafeatures2d`) non résolus. Cause : la cible de test linkait un sous-ensemble OpenCV figé (`core/imgproc/calib3d/features2d`) alors que le refactor Phase 3 ajoute video (optical flow) + cuda (ORB GPU, compilé car `IBOM_HAVE_OPENCV_CUDA=1` est global). **Fix** : `test_tracking_worker` linke `${OpenCV_LIBS}` (comme la cible principale) + ajout du composant `video` à `find_package(OpenCV)` racine. [JETSON_ERREURS #50](JETSON_ERREURS.md#erreur-50--test_tracking_worker-ne-linke-pas-modules-opencv-manquants). Fichiers : `tests/CMakeLists.txt`, `CMakeLists.txt`. **Action utilisateur** : reconfigurer + rebuild — un ajout de composant `find_package` nécessite une **reconfiguration CMake** (`scripts/build_jetson.sh` la fait ; sinon `rm -rf build/CMakeCache.txt build/CMakeFiles` puis re-cmake). Coller la prochaine erreur si le link/`ctest` bute encore — sinon on a un build complet vert (binaire déjà OK, restent les tests).

## État actuel — au 2026-06-20 (1er build Jetson de PR #20 — fix déclaration dupliquée)

> **2026-06-20 (suite 90)** : **premier build Jetson de la PR #20** lancé par l'utilisateur (`bash scripts/build_jetson.sh`, CMake résumé `RealSense: ON`, `IBOM_HAVE_OPENCV_CUDA=1`). **Échec à `[27-30/89]`** sur toute cible incluant `TrackingWorker.h` : `processIncremental` **déclaré deux fois de suite** (lignes 161 et 163) — artefact de copier-coller du refactor Phases 1-3, jamais attrapé car PR #20 n'avait jamais été compilée. **Fix** : suppression de la paire commentaire+déclaration en double dans `src/overlay/TrackingWorker.h` (le `.cpp` ne la définissait qu'une fois → header seul en faute). [JETSON_ERREURS #49](JETSON_ERREURS.md#erreur-49--processincremental-declare-deux-fois-build-pr-20). Vérif rapide effectuée par relecture : `processIncremental`/`processReference`/`runOpticalFlow`/`seedFlowLandmarks`/`detectFeatures` chacune définie **une seule fois** dans le `.cpp` ; le bloc GPU `detectFeatures` (`#ifdef IBOM_HAVE_OPENCV_CUDA`, `cv::cuda::ORB::detectAndCompute` + upload/download) est laissé tel quel — overload plausible mais **non vérifiable sans toolchain** (déjà signalé suite 88 comme point à valider). ⚠️ Le build ne s'était arrêté qu'à **cette première erreur** : d'autres erreurs de compilation de PR #20 (surtout le chemin GPU Phase 3, +4183 lignes jamais compilées) peuvent encore surgir. **Action utilisateur** : relancer `bash scripts/build_jetson.sh` après ce fix et coller la prochaine erreur éventuelle — on itère. Fichier : `src/overlay/TrackingWorker.h`.

## État actuel — au 2026-06-20 (PR #20 ouverte — en attente de validation Jetson)

> **2026-06-20 (suite 89)** : les 3 phases du [plan Live Tracking](LIVE_TRACKING_PLAN.md) sont livrées (Phase 1 suite 86, Phase 2 suite 87, Phase 3 suite 88), ainsi que les fixes Multi-Comp (pin 1 rouge + `componentAtPcb` suite 83, opposite-pads rouge suite 84). Tout est commit/push sur `claude/pensive-euler-pvde0v`. **Ouverture de la Pull Request** [#20](https://github.com/lo26lo/Assistant/pull/20) (`claude/pensive-euler-pvde0v` → `main`) à la demande de l'utilisateur (« commit le pr »).
>
> **Prochaine étape obligatoire avant merge** : valider sur le Jetson AGX Orin + RealSense D405 (build Docker complet — risque build plus élevé que d'habitude à cause du chemin GPU CUDA de la Phase 3, cf. checklist suite 88) :
> 1. Build → vérifier le message CMake `OpenCV CUDA: ON/OFF` cohérent avec l'attendu.
> 2. Live tracking scène statique → overlay immobile (plus de vibration).
> 3. PCB Map → clic sur composant dense → sélection fiable ; Pin 1 et Opposite-Pads → marqueurs rouges identiques.
> 4. Tester progressivement les nouveaux réglages Settings → Tracking (Motion model, 1€ filter, CLAHE, Optical-flow, GPU mode) — défauts = comportement legacy inchangé.
>
> Aucune modification de code dans cette suite (administratif : ouverture PR + journal). Si une session future reprend ce travail, commencer par lire ce bloc puis l'historique suite 82→88 ci-dessous, et vérifier l'état de la PR #20 sur GitHub (CI, reviews, merge status).

## État actuel — au 2026-06-19 (Live Tracking — Phase 3 : optical flow + CLAHE + GPU Jetson)

> **2026-06-19 (suite 88)** : **implémentation Phase 3** du [plan](LIVE_TRACKING_PLAN.md) (« phase 3 »). 3 fonctionnalités, **toutes derrière des flags, défaut conservateur** :
> - **3.1 Optical flow hybride (Lucas-Kanade, CPU)** : `runOpticalFlow()` + `seedFlowLandmarks()`. En reference mode, après un fit ORB réussi, les keypoints (déjà masqués sur la carte) sont rétro-projetés en coords PCB via H⁻¹ → landmarks `(m_flowPcb ↔ m_flowImg)`. Les frames suivantes (jusqu'à `m_flowRedetectInterval=30`) **ne refont pas d'ORB** : elles trackent les landmarks frame→frame par `cv::calcOpticalFlowPyrLK` (sub-pixel, fluide, peu coûteux) et refit PCB→image via `estimateModel`. ORB se rafraîchit périodiquement ou si trop de points perdus. Drift-free par frame (chaque fit est PCB→image direct, pas d'accumulation). Défaut **off**.
> - **3.2 CLAHE** : `cv::createCLAHE(2.0, 8×8)` appliqué sur le gris pleine résolution avant ORB/flow → keypoints plus stables sous glare/éclairage inégal (D405). Défaut **off**.
> - **3.3 Accélération GPU Jetson** : `detectFeatures()` route la détection ORB sur `cv::cuda::ORB` quand dispo, sinon CPU. **Compilation conditionnelle** `#ifdef IBOM_HAVE_OPENCV_CUDA` (macro définie par CMake si modules `cudafeatures2d`/`cudaimgproc` trouvés — Jetson oui, Windows-legacy non → CPU). Descripteurs téléchargés en CPU pour réutiliser le matching/bucketing/subpix existants (intégration minimale, moins risquée). Runtime : actif si `getCudaEnabledDeviceCount()>0` et mode≠off ; fallback CPU automatique sur exception. Défaut mode **auto**.
>
> **Plomberie** : nouveau slot `TrackingWorker::setAdvanced(clahe, opticalFlow, gpuMode)` invoqué aux 2 sites configure d'`Application`. Config : `m_trackingClahe`(false)/`m_trackingOpticalFlow`(false)/`m_trackingGpuMode`(1) + accesseurs + JSON (`clahe`/`optical_flow`/`gpu_mode`). SettingsDialog onglet Tracking : checkbox CLAHE + checkbox Optical-flow + combo GPU acceleration (Off/Auto/Force). CMakeLists : `OPTIONAL_COMPONENTS cudafeatures2d cudaimgproc cudaarithm` + macro `IBOM_HAVE_OPENCV_CUDA` + message status. Reset flow dans `resetReference()`/`setAdvanced`.
>
> Fichiers : `src/overlay/TrackingWorker.{h,cpp}`, `src/app/Config.{h,cpp}`, `src/app/Application.cpp`, `src/gui/SettingsDialog.{h,cpp}`, `CMakeLists.txt`.
>
> ⚠️ **Non compilé/testé ici** — **risque build plus élevé que Phases 1-2** : le chemin GPU (`cv::cuda::ORB::detectAndCompute`) et les includes CUDA ne sont **pas vérifiables ici**. **À valider au prochain build Jetson** : (1) vérifier le message CMake « OpenCV CUDA: ON » ; (2) défaut (tout off/auto) → comportement Phase 2 inchangé, fallback CPU si GPU échoue (log « GPU ORB failed ») ; (3) activer **Optical-flow** → overlay plus fluide, surveiller la dérive entre rafraîchissements ORB (baisser `m_flowRedetectInterval` si besoin) ; (4) activer **CLAHE** sous glare ; (5) **GPU auto/force** → vérifier le gain FPS et l'absence d'erreur cuda (si l'overload `detectAndCompute` GPU ne compile pas, ajuster vers `detectAndComputeAsync`+download, ou passer gpu_mode=off). En cas de souci de compilation CUDA, le code CPU reste la voie par défaut.

## État actuel — au 2026-06-19 (Live Tracking — Phase 2 : 1€ Filter + modèle adaptatif + bucketing keypoints)

> **2026-06-19 (suite 87)** : **implémentation Phase 2** du [plan](LIVE_TRACKING_PLAN.md) (« go phase 2 »).
> - **2.1 1€ Filter** : nouveau header-only `src/overlay/OneEuroFilter.h` (filtre passe-bas à coupure adaptative à la vitesse, CHI 2012). `smoothHomography()` réécrit : au lieu de l'EMA à seuils (suite 82), filtre désormais **les 8 coordonnées des 4 coins board** avec un 1€ Filter chacun, puis refit l'homographie. Coupure basse au repos (jitter écrasé), s'ouvre instantanément en mouvement (pas de lag). Paramètres `m_oneEuroMinCutoff`/`m_oneEuroBeta`.
> - **2.2 Modèle de mouvement adaptatif** : nouveau `estimateModel()` (enum `Model` : Auto/Similarity/Affine/Homography) routé dans `processReference` + le re-lock hybrid. `Auto` fit **similitude ET homographie** et garde la similitude sauf si l'homographie est nettement meilleure (err < 0.7× ET ≥ autant d'inliers) → évite que le bruit fuie en fausse perspective sur carte plane. **Défaut = Homography (legacy, sûr)** : comportement inchangé tant que l'utilisateur ne choisit pas un autre modèle. Delta incrémental laissé en homographie (hors scope, risque compo).
> - **2.3 Distribution des keypoints** : `bucketKeypoints()` — grille 8×6, garde les plus forts par cellule (détecteur configuré à 2× la cible pour avoir le choix) → fit mieux conditionné sur toute la carte, moins de wobble aux bords.
>
> **Plomberie** : nouveau slot `TrackingWorker::setStabilization(model, minCutoff, beta)` invoqué aux 2 sites configure de `Application`. Config : champs `m_trackingModel`(=3)/`m_oneEuroMinCutoff`(=1.0)/`m_oneEuroBeta`(=0.02) + accesseurs + JSON load/save (`model`/`one_euro_min_cutoff`/`one_euro_beta`). SettingsDialog onglet Tracking : combo **Motion model** + spinbox **min cutoff (Hz)** + **beta**, avec tooltips. `OneEuroFilter.h` ajouté à `CMakeLists.txt`. Filtres remis à zéro dans `resetReference()` + à chaque `setStabilization`.
>
> Fichiers : `src/overlay/OneEuroFilter.h` (nouveau), `src/overlay/TrackingWorker.{h,cpp}`, `src/app/Config.{h,cpp}`, `src/app/Application.cpp`, `src/gui/SettingsDialog.{h,cpp}`, `CMakeLists.txt`.
>
> ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : (1) défaut (Homography + 1€) → overlay au moins aussi stable qu'avant, plus fluide en mouvement ; (2) passer Motion model → **Auto** ou **Similarity** sur la carte plane → tester si l'overlay est plus stable aux bords (et qu'un point de vue incliné reste correct en Auto) ; (3) ajuster min cutoff (baisser = plus stable au repos) / beta (monter = moins de lag) ; (4) surveiller le FPS (estimateModel en Auto fait 2 fits ; bucketing détecte 2× plus de keypoints — si trop lourd, repasser model=Homography et/ou baisser orbKeypoints). Si régression visible, model=Homography + beta défaut = comportement Phase 1.

## État actuel — au 2026-06-19 (Live Tracking — Phase 1 implémentée : statique + MAGSAC + subpix + gating)

> **2026-06-19 (suite 86)** : **implémentation Phase 1** du [plan Live Tracking](LIVE_TRACKING_PLAN.md) (« go » utilisateur). 4 changements dans `src/overlay/TrackingWorker.{h,cpp}` :
> - **1.1 Détection de scène statique** : nouveau `emitHomography()` — point d'émission unique qui compare le déplacement max des coins board (`cornerDisp()`) entre l'estimée courante et la **dernière émise** (`m_lastEmittedH`). Si < `m_staticThreshPx` (0.8px), **n'émet rien** → l'overlay se fige au lieu de scintiller. Anti-freeze permanent : l'ancre de comparaison est la dernière pose émise (pas accumulée), donc un mouvement lent finit toujours par dépasser le seuil (lag max ~0.8px).
> - **1.2 USAC_MAGSAC** : les 3 `findHomography(... cv::RANSAC ...)` (reference / hybrid re-lock / delta incrémental) passent à `cv::USAC_MAGSAC` (déterministe + plus précis) ; `cv::setRNGSeed(12345)` au constructeur.
> - **1.3 `cornerSubPix`** : nouveau `refineKeypointsSubPix()` appelé dans `processFrame()` après le rescale des keypoints en pleine résolution → réduit le jitter de quantification ORB à la source (fenêtre 5×5, no-op sur erreur).
> - **1.4 Gating qualité + hystérésis** : `emitHomography()` ne trust pas un fit avec `inliers < minMatchCount` → garde la dernière bonne pose (au lieu de sauter) ; passe `State::Lost` après 3 frames mauvaises consécutives (`m_lowQualityFrames`).
>
> `emitHomography()` enchaîne gate inliers → gate statique → `smoothHomography()` (lissage suite 82 conservé) → `emit`. `smoothHomography()` refactorisé pour réutiliser `cornerDisp()`. Nouveaux membres : `m_lastEmittedH`, `m_staticFrames`, `m_lowQualityFrames`, `m_staticThreshPx`. Reset dans `resetReference()`. **Cas utilisateur = reference mode** (log « reference captured (194 keypoints) ») → la Phase 1 s'y applique directement et proprement (recompute frais chaque frame, pas d'accumulation). Met à jour [JETSON_ERREURS #47](JETSON_ERREURS.md#erreur-47--overlay-vibre-en-live-tracking-sur-scene-statique-pas-de-lissage) (statut : lissage suite 82 + Phase 1 suite 86).
>
> ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** (protocole §6 du plan) : (1) caméra+carte fixes → overlay parfaitement immobile (gate statique) ; (2) mouvement lent → suit sans coller (lag ≤ ~1px) ; (3) main qui passe/glare → overlay fige au lieu de sauter (gate inliers) ; (4) vérifier le FPS (MAGSAC + cornerSubPix ajoutent un coût CPU — si trop lourd, réduire la fenêtre cornerSubPix ou limiter aux inliers). Réglages `m_staticThreshPx`/fenêtre subpix hardcodés pour l'instant → à exposer en Config (Phase 2). Tunables `USAC_MAGSAC` : si trop de rejets, repli `cv::RANSAC` possible.

## État actuel — au 2026-06-19 (Plan d'amélioration Live Tracking — docs/LIVE_TRACKING_PLAN.md)

> **2026-06-19 (suite 85)** : **demande utilisateur** — « est-ce qu'on peut encore améliorer le live tracking… fais-moi un plan de propositions », puis « j'aimerais un plan md, et regarde aussi sur le net… peut-être aussi utiliser le GPU du Jetson ». **Livré** : nouveau document **[docs/LIVE_TRACKING_PLAN.md](LIVE_TRACKING_PLAN.md)** — diagnostic des 3 sources de jitter (RANSAC randomisé, keypoints ORB quantifiés ×2 au downscale, sur-paramétrisation 8 DDL vs carte plane), revue d'état de l'art (recherche web : **1€ Filter** CHI 2012, motion smoothing IPOL 2017, optical flow LK planaire, ORB/optical-flow GPU sur Jetson), et **plan en 3 phases** :
> - **Phase 1** (faible risque) : détection scène statique → ne pas ré-estimer (tue le jitter à l'arrêt) ; `USAC_MAGSAC` déterministe ; `cornerSubPix` sur les matches ; gating qualité + hystérésis (fige au lieu de sauter).
> - **Phase 2** : 1€ Filter sur pose décomposée (tx,ty,θ,scale) ; modèle de mouvement adaptatif (similitude/affine/homographie auto) ; distribution spatiale des keypoints.
> - **Phase 3** : optical flow LK sub-pixel ; CLAHE anti-glare ; **accélération GPU** (`cv::cuda::ORB` / matcher / `SparsePyrLKOpticalFlow`).
>
> **Vérif faite** : OpenCV Jetson est compilé `WITH_CUDA=ON` + `opencv_contrib` arch 8.7 (`docker/base.Dockerfile`) → modules `cudafeatures2d`/`cudaoptflow` **disponibles**, la piste GPU est viable. Le doc liste aussi les réglages à exposer (Config/SettingsDialog) et un protocole de validation Jetson. **Aucune modif code** dans cette suite (planification pure). Recommandation : implémenter Phase 1 d'abord, une phase à la fois (validation matériel obligatoire — pas de toolchain ici). Fichier : `docs/LIVE_TRACKING_PLAN.md`.

## État actuel — au 2026-06-19 (Multi-Comp : méthode "pads opposés" en rouge comme pin 1)

> **2026-06-19 (suite 84)** : **retour utilisateur** — « attention il y a aussi le choix avec pin opposée qui doit avoir le même graphisme que pin 1 ». **Fix** : les cibles de clic de la PCB Map peuvent désormais être colorées — `BoardMinimap::setClickTargets()` prend un paramètre `QColor` (défaut vert). Le rendu des cibles utilise maintenant **le même graphisme que le marqueur pin 1** (halo noir + anneau coloré + croix + **point central plein**). La méthode **opposite pads** passe `QColor(255,70,70)` → 2 marqueurs **rouges** identiques au pin 1 (numérotés 1/2). La méthode **body corners** garde le vert (ce ne sont pas des pads). Texte de statut mis à jour (« two RED target pads »). Fichiers : `src/gui/BoardMinimap.{h,cpp}` (param couleur + dot central), `src/app/Application.cpp` (opposite-pads passe rouge, include `<QColor>`). ⚠️ **Non compilé/testé ici**. **À valider** : méthode opposite-pads → 2 anneaux rouges (même style que pin 1) sur les 2 pads extrêmes.

## État actuel — au 2026-06-19 (Multi-Comp : pin 1 en rouge + sélection composant fiable sur la PCB Map)

> **2026-06-19 (suite 83)** : **2 retours utilisateur**. (a) « pour le choix des pins je préférais la pin en rouge plutôt que la cible verte » ; (b) « on ne peut toujours pas cliquer sur le composant dans la map pour le sélectionner ».
>
> **Fix (a)** : pour la méthode **Pin 1**, ne plus poser d'anneau vert (`setClickTargets({})`) — on s'appuie sur le **marqueur rouge de la pin 1** déjà dessiné pour le composant sélectionné sur la PCB Map. Ce marqueur est rendu **plus proéminent** : halo noir + anneau rouge `(255,70,70)` rayon 6 + croix + point plein central, au lieu d'un simple petit point rayon 3. Texte de statut mis à jour (« the RED pin on the PCB Map » au lieu de « green target »). Les méthodes opposite-pads / body-corners gardent les cibles vertes (2 points, numérotées). Fichiers : `src/app/Application.cpp` (case 1 de `beginMarkComponent`), `src/gui/BoardMinimap.cpp` (rendu pin 1).
>
> **Fix (b)** [JETSON_ERREURS #48](JETSON_ERREURS.md#erreur-48--clic-pcb-map-ne-selectionne-pas-le-bon-composant-nearest-center-peu-fiable) : la sélection au clic sur la PCB Map prenait le composant dont le **centre** (`c.position`) était le plus proche du point cliqué — peu fiable sur carte dense (le centre d'un gros voisin peut être plus proche qu'un petit composant qu'on vise pile dessus). Nouveau helper `Application::componentAtPcb(pcbPt)` : **hit-test bbox** d'abord (le plus petit composant Front dont la bbox **contient** le point — donc cliquer *sur* la part la sélectionne), repli sur le centre le plus proche seulement si le clic tombe sur du board nu. Utilisé dans les 2 chemins de clic minimap (multi-align + sélection RealSense). Fichiers : `src/app/Application.{h,cpp}`.
>
> ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : en Multi-Comp, méthode Pin 1 → marqueur rouge proéminent sur la pin 1, plus d'anneau vert ; cliquer *sur* un composant dans la PCB Map (multi-align **et** hors multi-align sur D405) le sélectionne fiablement même sur zone dense.

## État actuel — au 2026-06-19 (Live Tracking : overlay qui "vibre" à l'arrêt — smoothing homographie)

> **2026-06-19 (suite 82)** : **retour utilisateur** (2 captures montrant le live tracking bien aligné : 4/4 inliers, erreur 0.000px, "Multi-align OK") — « c'est pas mal le live tracking mais ça vibre après si je ne bouge rien ». **Diagnostic** (lecture `TrackingWorker.{h,cpp}`) : chaque frame recalcule **de zéro** une homographie via ORB+RANSAC (`processReference`/`processIncremental`), sans aucun lissage temporel. Même sur une scène parfaitement statique, le bruit de localisation sub-pixel des keypoints ORB fait varier légèrement le fit RANSAC d'une frame à l'autre → l'overlay "vibre" visuellement de quelques pixels alors que rien ne bouge physiquement. Aucune logique existante n'amortit ce bruit ni ne distingue "petit bruit" de "vrai mouvement".
>
> **Fix** : nouvelle méthode `TrackingWorker::smoothHomography(rawH)` — projette `m_pcbPolygon` (les 4 coins du board bbox, déjà alimentés via `setBoardPolygon()` pour le masque ORB) à travers la **dernière estimée lissée** et la **nouvelle estimée brute**, mesure le déplacement max des coins projetés, puis :
> - déplacement ≤ 1.5px → bruit, blend très amorti (poids 0.15 sur la nouvelle estimée)
> - déplacement ≥ 12px → vrai mouvement, blend = 1.0 (nouvelle estimée intégrale, pas de lag)
> - entre les deux → rampe linéaire du poids
>
> Les points lissés sont ensuite refit en une homographie via `cv::findHomography(..., method=0)` (DLT least-squares, pas de RANSAC nécessaire — 4 points propres). Le lissage s'applique **uniquement à la valeur émise** (`homographyUpdated`) — les accumulateurs internes (`m_cumulativeH`, `m_lastHomography` utilisé pour le masque ORB) restent sur l'estimée brute, pour éviter de composer une erreur de lissage dans le suivi incrémental. Repli sans lissage si `m_pcbPolygon` a moins de 4 points (pas de référence géométrique). Reset dans `resetReference()` (nouveau membre `m_smoothedHomography`). Appliqué aux 3 points d'émission : `processReference`, le snap "hybrid" et la composition incrémentale normale dans `processIncremental`.
>
> Fichiers : `src/overlay/TrackingWorker.h` (nouveau membre `m_smoothedHomography`, déclaration `smoothHomography()`), `src/overlay/TrackingWorker.cpp` (implémentation + 3 sites d'appel).
>
> ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : activer Live Tracking sur une carte alignée, ne plus bouger la caméra/carte pendant 10-15s → l'overlay doit rester visuellement stable (plus de tremblement pixel par pixel) ; puis déplacer franchement la caméra → l'overlay doit suivre sans lag perceptible (vérifier que le seuil de snap à 12px n'introduit pas de retard gênant en usage réel — à ajuster si besoin).

## État actuel — au 2026-06-19 (Multi-Comp : panneau non-modal persistant + fix "Reset ne fait rien")

> **2026-06-19 (suite 81)** : **3 retours utilisateur** (capture : le QMessageBox modal de choix de méthode était ouvert, bloquant). (a) « quand je veux sélectionner le deuxième je n'ai plus la boîte de dialogue » ; (b) « je ne peux pas choisir non plus sur la mini map » ; (c) « j'aimerais aussi pour choisir corner, pin1, double pin dans la même configuration » ; + (d) « le reset alignement ne fait rien ».
>
> **Cause (a)+(b)** : le choix de méthode (suite 77) était un **QMessageBox modal** (`exec()`) — tant qu'il était ouvert il **bloquait toute interaction** (BOM, minimap, caméra). Et une fois fermé, plus aucune UI ne guidait la sélection des composants suivants. **Refonte** : nouveau **panneau non-modal persistant `gui::MultiAlignDialog`** (QDialog `Qt::Tool`, `setModal(false)`) qui reste ouvert pendant toute la collecte. Contenu : composant courant (« Marking: TR1 »), **3 radios de méthode commutables à tout moment** (Opposite pads / Pin 1 / Body corners → résout (c) : méthode **par composant**, mixable), ligne de statut/instructions, compteur de repères, boutons **« Finish & Align »** (activé à ≥2) et **« Cancel »**. Signaux `methodChanged/finishRequested/cancelRequested` câblés dans `Application`. Sélection composant (BOM **ou** minimap) → `beginMarkComponent()` met à jour le panneau + arme le clic ; changer de méthode ré-arme le composant courant immédiatement. La sélection minimap fonctionne désormais (plus de modal qui bloque). Fichiers **nouveaux** : `src/gui/MultiAlignDialog.{h,cpp}` (+ `CMakeLists.txt`). `Application.{h,cpp}` : membre `m_multiAlignDialog`, `showMultiAlignDialog()`, suppression du QMessageBox de démarrage, MAJ panneau dans `beginMarkComponent`/click handler, masquage dans `setMultiAlignUIState(false)`.
>
> **Cause (d) — bug réel** [JETSON_ERREURS #46](JETSON_ERREURS.md#erreur-46--reset-alignment-ne-fait-rien-overlay-fige) : le bloc de rendu overlay est gardé par `if (m_homography->isValid())` ; après Reset (`isValid()` false) le bloc est sauté, mais `CameraView` garde sa dernière image d'overlay et continue de la peindre → overlay **figé**, Reset semble inopérant. **Fix** : `else if (!m_pickingHomographyPoints) cameraView()->setOverlayImage(QImage())` pour effacer l'overlay résiduel. Fichier `src/app/Application.cpp`.
>
> ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : démarrer Multi-Comp → panneau non-modal s'ouvre (ne bloque pas) ; cliquer un composant dans la PCB Map **ou** le BOM → « Marking: X » + repères verts ; changer le radio de méthode → ré-arme ; marquer ≥2 → « Finish & Align » actif ; **Reset Alignment** → overlay disparaît immédiatement.

## État actuel — au 2026-06-19 (Auto-Align : avertissement explicite quand le score est faible)

> **2026-06-19 (suite 80)** : **test réel utilisateur** (capture) — carte FOC_Stim **surélevée** (depth fill 87%, distance 91mm), mais Auto-Align passe quand même **via contour (score 0.34)** et l'overlay est **visiblement décalé à droite** (les pads jaunes ne couvrent pas le vrai PCB vert). « j'ai surélevé mais marche pas mieux pour l'auto ». **Diagnostic** : le `scoreOrientation()` (recouvrement edge map dilatée ↔ empreintes iBOM projetées) est **peu discriminant** sur un PCB dense devant un fond encombré/réfléchissant (bois + ombre + zone granuleuse à droite) : un quad spatialement décalé recouvre quand même ~1/3 des arêtes réelles → score 0.34, au-dessus du seuil d'acceptation `kMinAcceptableScore=0.10`, donc appliqué silencieusement comme « succès » alors qu'il est faux. Sessions passées (suites 63/64/65/69) confirment qu'Auto-Align est structurellement non fiable pour CETTE carte (glossy → depth bruitée ; fond chargé → contour piégé). **Décision** : **ne PAS** tenter de retoucher la CV en aveugle (pas de toolchain pour valider, risque de régression). À la place, **avertissement honnête** : nouveau seuil `kAutoAlignTrustScore=0.45` dans `autoAlignBoard()` — sous ce score, le résultat est appliqué mais **signalé LOW confidence** (message de statut + `QMessageBox::warning`) qui explique pourquoi et **oriente vers le chemin fiable** : « Reset Alignment » puis « Align: Multi-Comp » (désormais très amélioré : repères verts PCB Map, sélection fluide, méthode 2-pads, persistance). Au-dessus de 0.45, message de succès inchangé. Fichier : `src/app/Application.cpp`. ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : sur cette carte, Auto-Align doit maintenant afficher l'avertissement LOW confidence (score ~0.34) au lieu d'un faux « succès » ; le multi-comp reste la voie recommandée. **Note** : si l'utilisateur veut vraiment fiabiliser Auto-Align, piste non faite (à tester) = biais de centrage dans `locateViaContour()` (préférer un quad proche du centre visé / du centroïde du plan depth) — risqué sans frames de test, donc reporté.

## État actuel — au 2026-06-19 (Nouveau bouton "Reset Alignment")

> **2026-06-19 (suite 79)** : **demande utilisateur** — « j'aimerais aussi un bouton pour faire un reset ». **Implémenté** : bouton **« Reset Alignment »** dans le groupe Alignment du panneau Controls (sous Auto-Align). Handler `resetAlignmentRequested` : annule tout flux d'alignement en cours (4-coins/2-comp/multi-comp/anchor microscope), `m_homography->reset()` + `m_overlayRenderer->setHomography()` (overlay redevient non-aligné), `m_currentPixelsPerMm = 0` + reset `StatsPanel`, `++m_alignmentEpoch`, et **efface aussi le profil sauvegardé** (`Config::clearSavedAlignment()` + `save()`) pour qu'il ne soit plus proposé au prochain chargement de cet iBOM (cf. suite 76 — persistance). Message de statut explicite invitant à relancer un alignement. Réutilise `setMultiAlignUIState(false)` pour nettoyer aussi les repères verts de la PCB Map (suite 78) si un multi-align était en cours. Fichiers : `src/gui/ControlPanel.{h,cpp}` (bouton + signal), `src/app/Application.cpp` (handler). ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : aligner la carte, cliquer « Reset Alignment » → overlay redevient non-aligné/centré par défaut ; fermer/rouvrir l'app avec le même iBOM → l'ancien alignement n'est plus restauré (profil bien effacé).

## État actuel — au 2026-06-19 (Multi-Comp : repères verts "cliquer ici" sur la PCB Map)

> **2026-06-19 (suite 78)** : **demande utilisateur** — « fait apparaître les pads sur la minimap sur lesquels on doit cliquer pour calibrer ». **Implémenté** : nouvelle API `BoardMinimap::setClickTargets(const std::vector<cv::Point2f>& pcbPts)` qui dessine des **repères verts vifs proéminents** (anneau vert `(50,230,90)` + halo noir + croix, et un **numéro** quand il y en a plusieurs) aux points PCB exacts à cliquer dans l'image caméra. Alimentée par `Application::beginMarkComponent()` selon la méthode de session : **pin 1** → 1 repère sur le pad pin 1 ; **opposite pads** → 2 repères sur les 2 pads les plus éloignés (`pa`/`pb`) ; **body corners** → 2 repères sur les coins diagonaux du bbox. Nettoyage : `setClickTargets({})` appelé dans les branches d'abandon (pas de pin 1 / <2 pads), après qu'un composant est complètement marqué (clic image final), et centralisé dans `setMultiAlignUIState(false)` (couvre fin + annulation croisée 4-coins/2-comp). Messages de statut mis à jour (« green target »). Fichiers : `src/gui/BoardMinimap.{h,cpp}` (setter + membre `m_clickTargets` + dessin), `src/app/Application.cpp` (appels dans `beginMarkComponent`, click handler, `setMultiAlignUIState`). ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : sélectionner un composant en Multi-Comp → 1 ou 2 anneaux verts numérotés apparaissent sur la PCB Map aux pads/coins à cliquer ; après les clics image correspondants, les repères disparaissent ; changer de composant déplace les repères.

## État actuel — au 2026-06-19 (Multi-Comp : sélection depuis la PCB Map + flux fluide + méthode "2 pads opposés")

> **2026-06-19 (suite 77)** : **2 demandes UX + 1 question précision de l'utilisateur**. (a) « je désire qu'on puisse sélectionner le composant dans la minimap plutôt que dans l'onglet BOM » ; (b) « aujourd'hui ça ouvre le message de pin 1 et angle mais je ne peux pas facilement changer de composant sans faire cancel, j'aimerais qu'on puisse le faire avec le popup qui reste ouvert » ; (c) « je me demande si on ne serait pas plus précis en mettant deux pins opposées plutôt que pin 1 ? ».
>
> **Réponse précision (c)** : **oui**, deux pads opposés est plus précis que pin 1 — (1) l'ancre PCB devient le **milieu de deux positions de pads connues** (iBOM) au lieu d'un point unique, (2) deux clics image qui chacun se "snappent" sur du cuivre via le `cv::cornerSubPix` ajouté en suite 76 → l'erreur de clic aléatoire se compense partiellement. Pour les passifs 2 pads (R/C/D, majorité d'une carte) les "2 pads les plus éloignés" sont non ambigus ; pour les CI ce sont les broches en coin diagonal.
>
> **Refonte du flux Multi-Comp** (résout a + b + c) : **la méthode de marquage est désormais choisie UNE SEULE FOIS** au démarrage (QMessageBox unique : « Opposite Pads (recommended) » / « Pin 1 » / « Body Corners ») au lieu d'un popup modal **par composant** qui bloquait la re-sélection. Ensuite, sélectionner un composant — **depuis le BOM panel OU la PCB Map** — appelle le nouveau helper `Application::beginMarkComponent(ref)` qui calcule l'ancre PCB selon la méthode de session et arme le clic, **sans popup**. **Switch libre** : re-sélectionner un autre composant avant d'avoir fini les clics ne fait que ré-armer (plus besoin de Cancel). **Nouvelle méthode `m_alignMultiMethod == 2`** (2 pads opposés) : ancre PCB = milieu des **2 pads les plus éloignés** du composant (calcul O(n²) sur les pads), 2 clics image → milieu (même mécanique que la méthode "coins" mais ancre = milieu des pads au lieu du centre bbox). **Sélection minimap** : le handler `BoardMinimap::anchorRequested` intercepte d'abord `if (m_alignMulti)` → composant le plus proche (Front) → `beginMarkComponent` (avant la logique re-anchor microscope / highlight RealSense existante). Fichiers : `src/app/Application.{h,cpp}` (helper `beginMarkComponent`, refonte handlers BOM/minimap/start/clic, `m_alignMultiMethod` doc), `src/gui/AlignmentWizard.cpp` (instructions page Run Multi-Comp mises à jour). ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : démarrer Multi-Comp → 1 popup de méthode ; cliquer un composant dans la PCB Map → il s'arme directement (pas de popup) ; cliquer un autre composant avant de cliquer l'image → switch sans Cancel ; méthode "Opposite Pads" → cliquer les 2 pads extrêmes → milieu apparié ; vérifier que l'alignement final est au moins aussi bon qu'avec pin 1.

## État actuel — au 2026-06-19 (Persistance de l'alignement + raffinement sub-pixel des clics Multi-Comp)

> **2026-06-19 (suite 76)** : **2 dernières demandes utilisateur du même message** (suite au succès du multi-align 2/2 sur FOC_Stim) : (3) « ce serait bien que le profil d'alignement soit sauvegardé si on ferme et qu'on revient sans bouger » ; (4) « on pourrait encore implémenter une fonction auto pour essayer d'être encore plus précis lors qu'on a fait alignement multi click ».
>
> **Implémenté (3) — persistance** : nouveau bloc `Config::SavedAlignment` (`valid`, `ibomFilePath`, `matrix[9]` homographie 3×3 row-major, `pixelsPerMm`, `timestamp` ISO 8601) sérialisé sous `"last_alignment"` dans le JSON de config. **Sauvegarde** : à chaque alignement réussi, `Application::reportAlignmentResult()` (déjà le point de passage commun des 4 méthodes) capture `m_homography->matrix()` + `m_currentPixelsPerMm` + le chemin iBOM actif et appelle `m_config->save()`. **Restauration** : dans `Application::loadIBomFile()`, juste avant le fallback "homographie centrée par défaut" — si `sa.valid` et `sa.ibomFilePath == path` (même fichier iBOM), on restaure la matrice via `m_homography->setMatrix()` + le scale px/mm, on incrémente `m_alignmentEpoch`, et on affiche un message explicite « Restored previous alignment for this board (saved …) — re-align if the camera or board has moved. » — **honnêteté volontaire** : aucune détection réelle de mouvement n'est possible, donc c'est présenté comme un point de départ à vérifier, pas une garantie. Fichiers : `src/app/Config.{h,cpp}` (struct + load/save JSON), `src/app/Application.cpp` (capture dans `reportAlignmentResult`, restauration dans `loadIBomFile`).
>
> **Implémenté (4) — raffinement sub-pixel** : nouvelle méthode `Application::refineClickPoint(cv::Point2f, int searchRadiusPx=8)` — convertit `m_lastColorFrame` (déjà tenu à jour à chaque frame) en niveaux de gris et lance `cv::cornerSubPix` dans une petite fenêtre de recherche autour du clic brut ; si le résultat est non-fini ou s'éloigne de plus de `searchRadiusPx` du clic original (corner non fiable, ex. centre de pad plat sans texture), **on garde le clic brut** (pas de snap hasardeux vers une mauvaise feature voisine sur une carte dense). Appliqué dans le handler de clic Multi-Comp (`CameraView::clicked`), sur **chaque** clic (les 2 coins ET le clic pin 1) avant de les empiler dans `m_alignMultiImgPts`. Log `spdlog::debug` de l'écart en pixels pour diagnostic. Scope volontairement limité au flux Multi-Comp (seule méthode explicitement visée par la demande "alignement multi click") — pas appliqué aux clics 4-coins/2-composants. Fichiers : `src/app/Application.{h,cpp}`.
>
> ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : (3) faire un alignement multi-comp, fermer l'app, la rouvrir avec le même iBOM → l'overlay doit apparaître déjà aligné avec le message "Restored previous alignment…" ; changer manuellement l'angle/position de la carte doit quand même restaurer l'ancien alignement (comportement attendu, pas un bug — l'utilisateur doit re-aligner) ; (4) en Multi-Comp, cliquer un coin proche d'une arête nette de silkscreen → vérifier en log debug (`refineClickPoint: (...) -> (...) [...px]`) que le point est légèrement ajusté ; cliquer au centre d'un pad uniforme doit garder le clic brut (pas de message de rejet visible à l'utilisateur, juste en debug log).

## État actuel — au 2026-06-19 (Multi-Comp : texte bouton "Finish Align" + auto-switch tab BOM)

> **2026-06-19 (suite 75)** : **2 demandes UX de l'utilisateur** suite au test réel (multi-align réussi 2/2 sur FOC_Stim, scale 6.21 px/mm) : (1) « il faut que quand on clique sur start multi-component, le texte change à finish align » — le bouton ne changeait pas de libellé pendant la collecte de landmarks, seul le statut texte l'indiquait ; (2) « il faut aussi que ça switch automatiquement sur le tab bom » — il fallait chercher manuellement l'onglet BOM (tabifié avec Controls) pour sélectionner un composant. **Implémenté (1)** : `ControlPanel::setAlignMultiActive(bool)` (toggle `m_btnAlignMulti` entre « Align: Multi-Comp (Beta) » et « Finish Align: Multi-Comp ») + `AlignmentWizard::setMultiAlignCollecting(bool)` (même toggle sur le bouton de la page Run du wizard, via `AlignRunPage::setCollecting()`) + helper unificateur `Application::setMultiAlignUIState(bool collecting)` qui appelle les deux (évite la désync bouton manuel ↔ wizard). Câblé aux 4 sites pertinents : `applyMultiAlignment()` (fin → false), démarrage 4-corners (annulation croisée → false), démarrage 2-composants (annulation croisée → false), démarrage multi-comp (→ true). **Implémenté (2)** : `MainWindow::showBomPanel()` (raise du dock BOM, désormais membre `m_bomDock` au lieu d'une variable locale dans le setup des docks) appelé au démarrage du mode 2-Components **et** Multi-Comp (les deux dépendent de la sélection BOM). Fichiers : `src/gui/ControlPanel.{h,cpp}`, `src/gui/AlignmentWizard.{h,cpp}`, `src/gui/MainWindow.{h,cpp}`, `src/app/Application.{h,cpp}`. ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : cliquer « Align: Multi-Comp » → bouton affiche « Finish Align: Multi-Comp » + l'onglet BOM panel passe au premier plan automatiquement ; idem pour « Align: 2 Components » (switch tab seulement, pas de toggle texte) ; re-clic ou fin de collecte → bouton revient à son texte initial.
>
> **Reste à faire (demandes 3 et 4 du même message utilisateur, non commencées)** : (3) persister le profil d'alignement (homographie + scale) pour le restaurer si on ferme/rouvre l'app sans avoir déplacé carte/caméra — piste : nouveau bloc JSON dans `Config` (matrice 3×3 + chemin iBOM associé + horodatage), sauvegardé à chaque succès d'alignement (sites déjà routés via `reportAlignmentResult()`), restauré dans `loadIBomFile()` si le chemin correspond, avec avertissement explicite que c'est heuristique (aucune détection de mouvement réel). (4) fonction auto pour raffiner la précision après alignement multi-clic — piste : `cv::cornerSubPix` autour de chaque point cliqué (méthode coins) avant de calculer la transformation finale dans `applyMultiAlignment()`.

## État actuel — au 2026-06-19 (Fix détection pin 1 iBOM — `pin1` entier non lu)

> **2026-06-19 (suite 74)** : **bug trouvé via test réel** — en multi-align, « Pin 1 » sur U7 (ESP32) → « U7 has no pin-1 pad in the iBOM ». Pourtant l'iBOM connaît la pin 1 (pointée par l'utilisateur dans le viewer). **Cause** : `IBomParser::parsePads()` ne lisait `pin1` que s'il était **booléen**, or l'iBOM l'encode en **entier** (`1`/`0`) → repli sur l'heuristique `num=="1"/"A1"` qui échoue pour un ESP32. **Fix** : lecture défensive de `pin1` (bool OU number≠0 OU string "1"/"true"). Fichier `src/ibom/IBomParser.cpp`. [JETSON_ERREURS #45](JETSON_ERREURS.md#erreur-45--pin1-ibom-entier-non-detecte). ⚠️ Non compilé/testé ici. **À valider** : « Pin 1 » sur U7 doit désormais accepter et demander de cliquer la pin 1.

## État actuel — au 2026-06-19 (Sélection composant : emphase forte dans l'overlay caméra + halo minimap)

> **2026-06-19 (suite 73)** : **retour utilisateur sur le repère de sélection** — sur board dense, le rectangle d'un petit CMS (ex. TR1) ne fait que 1-2 px dans la PCB Map → invisible. D'abord ajout d'un **halo + croix de taille fixe** au centre du composant sélectionné dans `BoardMinimap` (indépendant de la taille du bbox). Puis l'utilisateur : « ou encore mieux, on le fait dans overlay, on a toutes les info et les pins » → **emphase forte dans l'overlay caméra** (`OverlayRenderer`). Nouvelle passe `drawSelectionEmphasis()` dessinée **en dernier (au-dessus de tout) et à pleine opacité** pour chaque composant surligné (`selectedRef` ou `m_highlightedRefs`) : boîte jaune vif épaisse (2.5px) autour du corps + **tous les pads** (cercles) avec la **pin 1 en rouge, plus gros (6px)** — affiché **quels que soient** les toggles pads/pin1, donc le composant est repérable directement sur l'image live (utile pour choisir les landmarks d'alignement). Gère homographie valide (`transformRect`/`pcbToImage`) ou non (coords PCB brutes en fallback). Aucun nouveau câblage : `setHighlightedRefs({ref})` est déjà appelé à la sélection BOM → l'emphase apparaît à la frame suivante. Fichiers : `src/overlay/OverlayRenderer.{h,cpp}` (méthode + 2e boucle dans `render()`), `src/gui/BoardMinimap.cpp` (halo+croix). **Build OK** (linking `MicroscopeIBOM` réussi). **Retour utilisateur post-build** : le repère est bien visible **dans la PCB Map** (halo+croix) mais l'emphase overlay caméra ne se voit pas tant qu'aucune homographie n'est posée (composants dessinés en coords PCB brutes hors-écran avant alignement — normal). Couleur changée **jaune → rouge foncé** (`(210,30,30)` minimap halo+rect sélectionné, `(170,0,0)` boîte overlay + fill translucide alpha 70) car le jaune se confondait avec le silkscreen/cuivre. Pads de guidage minimap restent jaunes, pin 1 reste rouge. Les pads du composant sélectionné dans l'overlay sont désormais **remplis en rouge** (alpha 220) pour couvrir le rendu cyan normal des pads (sinon le cyan transparaît → "c'est bleu"). ⚠️ Reste à valider l'emphase overlay **après** un alignement (homographie valide).

## État actuel — au 2026-06-19 (Nouveau : Assistant d'alignement — wizard guidé avec résumé)

> **2026-06-19 (suite 72)** : **feature demandée par l'utilisateur** — « un wizard serait bien, on clique sur Alignement et on arrive sur une popup avec tous les choix et on peut faire suivant suivant avec à la fin le résumé de ce que l'on a obtenu ». **Implémenté** : nouvelle classe `gui::AlignmentWizard` (sous-classe `QWizard`, 3 pages) + bouton **« Alignment Assistant… »** en tête du groupe Alignment (les 4 boutons manuels restent pour les experts). Pages : (1) **choix de méthode** (radios : Auto-Align / 4 Corners / 2 Components / Multi-Comp, chacune avec description ; la **recommandée est présélectionnée** selon le backend — RealSense→Auto-Align, microscope→2 Components) ; (2) **exécution** : instructions spécifiques à la méthode + bouton « Start alignment » + zone de statut live ; (3) **résumé** : affiche le texte du résultat obtenu. **Architecture clé** : le wizard est **non-modal** (`setModal(false)` + `show()`) car l'alignement reste **interactif** (clics dans la caméra / BOM panel) — il ne fait qu'**orchestrer**. Son signal `startRequested(int method)` est relié (signal→signal) aux signaux existants du `ControlPanel` (`alignHomographyRequested`/`alignOnComponentsRequested`/`alignMultiRequested`/`autoAlignRequested`), donc **zéro duplication** de la logique d'alignement. Le retour de résultat passe par un nouveau helper `Application::reportAlignmentResult(QString)` qui met à jour la barre de statut **et** (si le wizard est visible) appelle `wizard->reportResult()` → la page d'exécution devient « complete » (`completeChanged()`) → Next activé → page résumé. Les 4 sites de succès (`applyMultiAlignment`, `autoAlignBoard`, fin 2-comp, fin 4-corner) routés via ce helper. Fichiers : **nouveaux** `src/gui/AlignmentWizard.{h,cpp}` (+ `CMakeLists.txt`), `src/gui/ControlPanel.{h,cpp}` (bouton + signal `alignmentWizardRequested`), `src/app/Application.{h,cpp}` (membre `m_alignWizard`, handler de lancement, helper `reportAlignmentResult`, include). ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : clic « Alignment Assistant… » → wizard s'ouvre, méthode recommandée présélectionnée ; choisir une méthode → page d'instructions ; « Start » → faire les clics → la page se complète et affiche le résultat ; Next → page résumé avec le texte (model/scale/erreur). Vérifier que le wizard non-modal **ne bloque pas** les clics dans la vue caméra.

## État actuel — au 2026-06-19 (Multi-align : repère visuel pin 1 + pads dans la PCB Map + UX clarifiée)

> **2026-06-19 (suite 71)** : **clarification UX demandée par l'utilisateur** — « rien n'est clair pour l'allignement du multi composant. il faut sélectionné le composant sur la mini map, sur l'overlay (par exemple pour la pin 1) et on fait comment après[?] les coins opposé c'est toujours les même, de gauche à droite ? Comment c'est géré avec les empreintes de esp32 par exemple. c'est le silkscreen ou… ». Réponses : (1) la sélection se fait **uniquement dans le BOM panel** (même mécanisme que « Align: 2 Components »), pas la minimap ni l'overlay ; après « Pin 1 », le **prochain clic** dans l'image = position de la pin 1 ; (2) pour les 2 coins opposés il **n'y a aucune convention** d'ordre — n'importe quelle diagonale convient, le code ne calcule que le **milieu** (`mid = (corner1 + corner2)/2`) ; (3) « le corps du composant » n'est **PAS** dérivé du silkscreen/footprint iBOM — c'est un clic **à l'œil** sur le contour visible, donc ambigu pour un module ESP32 (antenne, blindage, plusieurs rangées). → recommandation : utiliser **« Pin 1 »** pour les empreintes complexes (ancré sur `pad.position` iBOM, précis). **Implémenté (repère visuel)** : la **PCB Map (BoardMinimap)** dessine maintenant, pour le composant sélectionné, **tous ses pads** (points jaunes translucides) et **marque la pin 1** (gros point rouge plein) — permet de lire l'orientation du composant et localiser la pin 1 sur la carte réelle avant de cliquer dans l'image. Aucun câblage supplémentaire : le minimap a déjà tout le projet et utilise `m_selectedRef` (déjà positionné lors de la sélection BOM). Messages de statut enrichis (renvoient à la « PCB Map » : point rouge = pin 1, contour pour les coins ; « order doesn't matter ») + tooltip du bouton « Align: Multi-Comp » réécrit en 3 étapes explicites (BOM → choix méthode → répéter → re-clic pour terminer). Fichiers : `src/gui/BoardMinimap.cpp` (bloc de dessin pads+pin1 du composant sélectionné dans `paintEvent`), `src/app/Application.cpp` (messages statut pin 1 / coins), `src/gui/ControlPanel.cpp` (tooltip bouton). ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : démarrer Multi-Comp, cliquer un composant dans le BOM → la PCB Map doit afficher ses pads en jaune + la pin 1 en rouge ; les messages de statut doivent guider clairement (clic pin 1 / 2 coins quelconques).

## État actuel — au 2026-06-19 (Nouveau : alignement multi-composants pour cartes non rectangulaires)

> **2026-06-19 (suite 70)** : **feature demandée par l'utilisateur** — sa carte (FOC_Stim) n'est **pas rectangulaire** (encoches/contour irrégulier) donc « Align: 4 Corners » est inutilisable (pas de vrais coins). Il a demandé comment marche « Align: 2 Components » et proposé de cliquer les 4 coins d'un composant. Expliqué : le 2-comp utilise les **centres** de 2 composants → transformation de **similarité** (4 DDL, pas de perspective), la forme de la carte est sans importance ; cliquer les coins d'**un** composant serait mal conditionné (coins trop proches). Proposé via AskUserQuestion → choix **« Align N composants (centres) »**, puis l'utilisateur a soulevé « difficile de trouver exactement le milieu sans repère » → 2e question sur le repérage → réponse **« on peut choisir entre 1 et 2 »** (les deux méthodes : 2 coins opposés→milieu, OU pin 1). **Implémenté** : nouveau mode **« Align: Multi-Comp (Beta) »** (bouton sous « Align: 2 Components »). Flux : clic bouton = démarrer la collecte ; pour chaque composant, sélection dans le BOM → **QMessageBox** « 2 Opposite Corners » / « Pin 1 » ; méthode coins = 2 clics image → **milieu** apparié au **centre bbox** (PCB) ; méthode pin 1 = 1 clic image → apparié à la **position du pad isPin1** (PCB, plus précis). Re-clic du bouton = **terminer & calculer**. Fit selon le nombre de repères : **≥4 → homographie** (`cv::findHomography` RANSAC, corrige la perspective/inclinaison) ; **3 → affine** (`cv::estimateAffine2D`) ; **2 → similarité** (math existante). Puis projection des 4 coins du board bbox par le transform → chemin standard `Homography::compute()` + scale px/mm depuis l'arête haute + reset tracking + `m_alignmentEpoch++`. Fichiers : `src/app/Application.{h,cpp}` (membres `m_alignMulti*`, méthode `applyMultiAlignment()`, handlers BOM/clic image, annulation croisée avec les autres modes), `src/gui/ControlPanel.{h,cpp}` (bouton + signal `alignMultiRequested`). ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV). **À valider au prochain build Jetson** : marquer 4 composants répartis (coins ou pin 1), cliquer le bouton pour terminer → overlay posé via homographie, log `Multi-align OK: 4 components, homography model`. **Note** : avec ≥4 points c'est le **seul** chemin manuel qui corrige la perspective (les autres sont similarité/affine), donc le meilleur pour une carte légèrement inclinée.

## État actuel — au 2026-06-19 (Auto-Align : course depth+contour quand le score profondeur est faible)

> **2026-06-19 (suite 69)** : **capture + log utilisateur** — Auto-Align sur D405 `succeeded via depth (score 0.13)` mais overlay visiblement décalé (déborde à gauche hors carte). Réponse utilisateur à ma question : « mais j'ai mis une feuille blanche dessous ». **Insight clé** : la feuille blanche n'aide **que** le contour 2D (contraste de luminance), pas la profondeur (aveugle à la couleur) — et `BoardLocator::locate()` essayait la profondeur d'abord, puis ne tentait le contour **que si la profondeur échouait** (`if depth … else contour`). Un succès **faible** de la profondeur (0.13, carte coplanaire avec la feuille → plan fusionne carte+feuille) masquait donc le contour : la feuille blanche était structurellement inutilisable. **Fix** : `locate()` réécrit en **course** — profondeur désambiguïsée d'abord ; si pas de résultat **ou** score < `kStrongScore = 0.30`, on lance aussi le contour et on garde le **meilleur score** des deux (remplace seulement si strictement supérieur + passe `kMinAcceptableScore`). Au-dessus de `kStrongScore` la profondeur est jugée fiable → contour sauté. Message d'échec combiné Depth+Contour. Fichier : `src/overlay/BoardLocator.cpp`. [JETSON_ERREURS #44](JETSON_ERREURS.md#erreur-44--auto-align-depth-faible-score-contour-jamais-essaye). ⚠️ Non compilé/testé ici. **À valider au prochain build Jetson** : avec feuille blanche sous la carte, Auto-Align doit pouvoir basculer sur le contour et produire un overlay mieux posé (score plus élevé) que le 0.13 profondeur ; vérifier dans le log `Board located via contour` quand le contour gagne.

## État actuel — au 2026-06-19 (BoardMinimap : clic = highlight composant sur RealSense, plus déplacement d'overlay)

> **2026-06-19 (suite 68)** : **rapport utilisateur + log terminal** — clic sur la minimap sur D405 : « ça fais bouger tout l overlay de la carte alors ça devrait higlité le cmposant ». Cause : `BoardMinimap::anchorRequested(pcbPoint)` était câblé dans `Application.cpp` (~ligne 2318) à un **re-ancrage 1-point** (recalcule toute l'homographie pour centrer le FOV caméra sur le point cliqué) — fonctionnalité pensée pour le **microscope à FOV étroit** (`docs/MICROSCOPE_PLACEMENT_PLAN.md`), où il faut effectivement recentrer la vue caméra puisque toute la carte n'est jamais visible. Sur RealSense (FOV large, carte entière déjà visible), ce comportement n'a pas de sens et donnait l'effet rapporté : un clic anodin fait sauter tout l'overlay. Demandé à l'utilisateur via AskUserQuestion → choix **« Highlight composant uniquement (RealSense) »**. **Fix** : le handler `anchorRequested` est maintenant branché en deux chemins selon `m_config->cameraBackend()` : **RealSense** → recherche linéaire du composant le plus proche (`Layer::Front`, distance euclidienne en mm) dans `m_ibomProject->components`, puis applique exactement le même effet qu'un clic dans le BOM panel (`m_overlayRenderer->setHighlightedRefs()`, `boardMinimap()->setSelectedRef()`, `bomPanel()->highlightComponent()`) — **aucune homographie touchée**. **Microscope (V4L2)** → comportement inchangé (anchor/recentrage, toujours pertinent à FOV étroit). Fichiers : `src/app/Application.cpp` (+`<limits>`). ⚠️ Non compilé/testé ici (pas de toolchain Qt6/OpenCV dans ce conteneur). **À valider au prochain build Jetson** : sur D405, cliquer la minimap doit surligner le composant le plus proche dans le BOM panel **sans** déplacer l'overlay/recalculer l'homographie ; sur microscope, le comportement de re-ancrage doit être inchangé.
>
> **Par ailleurs, signalé mais non traité cette session** (hors scope du rapport) : `Segmentation fault (core dumped)` à la sortie de l'app dans le même log (`Application exiting with code 0` suivi du crash) + warning Qt `QMainWindow::saveState(): 'objectName' not set for QToolBar 'Main'`. Le code de sortie 0 suggère un crash **pendant la destruction** après un retour propre du `main()` — pas encore investigué, à creuser si ça se reproduit ou si l'utilisateur le signale explicitement.

## État actuel — au 2026-06-19 (Tracking live : correction de dérive hybride beta)

> **2026-06-19 (suite 67)** : **case UI ajoutée** (« yes em dessous du live tracking »). Nouvelle checkbox **« Hybrid drift correction (beta) »** dans le panneau Controls, **sous** « Live Tracking Mode » (indentée `margin-left:18px`), pour activer/désactiver la feature suite 66 depuis l'UI. Signal `ControlPanel::hybridModeChanged(bool)` + getter/setter `hybridMode()`/`setHybridMode()`. Câblage `Application.cpp` : init de l'état depuis `m_config->hybridDriftCorrection()` (~152), connexion du toggle → `m_config->setHybridDriftCorrection()` + `save()` + `setHybridCorrection` sur le worker. État coché par défaut (= défaut config). Fichiers : `src/gui/ControlPanel.{h,cpp}`, `src/app/Application.cpp`. ⚠️ Non compilé/testé ici.

> **2026-06-19 (suite 66)** : **nouvelle feature beta demandée par l'utilisateur** (« ok est-ce qu on pourrait implémenter quelque chose en beta pour etre encore meilleur » → choix « Tracking hybride (anti-dérive) » via AskUserQuestion). Contexte : questions de l'utilisateur sur le suivi de l'overlay quand il bouge/incline la carte ; confirmé que `TrackingWorker` utilise ORB+RANSAC (vision classique, **PAS YOLOv8** — YOLO = `ai/ComponentDetector`, module séparé optionnel, `models/` vide), homographie projective complète → gère translation **et** inclinaison en principe, mais conditionné à l'activation de la checkbox **« Live Tracking Mode »** (déjà présente, `ControlPanel.cpp:232`). **Faiblesse traitée** : en mode incrémental (frame→frame, utilisé pour le microscope V4L2 FOV étroit), la dérive s'accumule (`m_accumulatedDrift`) et l'état `Drifting` ne déclenchait **aucune correction** — juste un flag UI. **Implémentation hybride** : en mode incrémental, on conserve désormais la **keyframe d'ancrage d'origine** (capturée au bootstrap : `m_refKeypoints`/`m_refDescriptors`, dont la `m_baseHomography` mappe PCB→cette image). À chaque frame, **avant** l'étape frame→frame, on tente de re-matcher la frame courante contre cette keyframe ; si match confiant (`rInliers >= m_minMatchCount` **et** `rErr <= m_ransacThreshold`), on **snap** `m_cumulativeH = refH * base` (estimation **sans dérive**), on remet `m_accumulatedDrift = 0`, état `Locked`. Sinon, fallback sur la composition incrémentale habituelle. Résultat : fluidité de l'incrémental sur grands mouvements + zéro dérive long-terme dès que l'ancrage redevient reconnaissable. Garde-fou anti-saut : on ne snap que sur un fit serré et bien supporté (sinon une mauvaise estimation d'ancrage remplacerait une meilleure estimation incrémentale). **Config** : `hybrid_drift_correction` (bloc `microscope`), **défaut `true`**, persisté. Slot `TrackingWorker::setHybridCorrection(bool)`, câblé aux 3 sites où `setIncrementalMode` est invoqué (`Application.cpp` : init ~393, switch backend ~629, apply settings ~1749). **N'a d'effet qu'en mode incrémental** (le mode référence D405 n'a pas de dérive). Fichiers : `src/overlay/TrackingWorker.{h,cpp}`, `src/app/Application.cpp`, `src/app/Config.{h,cpp}`. ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV dans ce conteneur). **À valider au prochain build Jetson** : en mode incrémental microscope, après un grand mouvement puis retour près de la pose d'origine, l'overlay doit se recaler sans dérive résiduelle (log `TrackingWorker[hybrid]: anchor re-locked … (drift reset)`), et l'état repasser `Locked`.

## État actuel — au 2026-06-18 (Auto-Align D405 intermittent : carte coplanaire avec la table)

> **2026-06-18 (suite 65)** : **log terminal complet obtenu** (la ligne Auto-Align était scrollée hors du screenshot). Numbers clés : Auto-Align **réussit puis échoue sur la même scène** sans réalignement — `21:04:04 Auto-Align succeeded via depth (score 0.26)`, puis `21:04:32` et `21:05:03` échouent avec `candidate quad area (356855 / 355897 px^2)` vs `(142237 px^2)` attendu. Ratio = **2.509×**, pile au-dessus de `kAreaTolerance = 2.5`. Scale 6.1 px/mm = fx 436.8 / distance 72mm → cohérent, l'aire attendue est juste. **Cause** : la carte est posée **à plat sur la table** → carte et table coplanaires (même distance) ; le masque de plan ±15mm (`kDepthBandMm`) englobe la carte **+ une marge de table** → quad ~1.58× trop grand en linéaire. Comme on est pile sur le seuil, le résultat dépend de la quantité de table coplanaire visible à l'instant t (cadrage) → intermittence. **Ne PAS desserrer le seuil** : accepter le quad carte+table mapperait les coins de la carte sur ceux de la table → overlay trop grand/mal placé (le symptôme « mal placé » des sessions passées). **Fix code** : message `validateSize()` rendu **directionnel** (trop grand → « merged with a coplanar background, lift the board / use manual alignment » ; trop petit → « make sure the whole board is in frame »). **Workaround utilisateur** : **surélever la carte** (petite boîte/support) pour qu'elle ressorte en profondeur → Auto-Align fiable ; ou alignement manuel 4-points (confirmé OK dans le log à 21:06). **Piste fix logiciel** (non faite, à valider avant) : raffiner le plan depth avec un signal couleur/arête pour isoler le PCB texturé du fond uniforme. Fichiers : `src/overlay/BoardLocator.cpp`, docs. [JETSON_ERREURS #41](JETSON_ERREURS.md#erreur-41--auto-align-d405-carte-coplanaire-avec-la-table). ⚠️ Non compilé/testé ici. **Self-cal** : le même log confirme le message OCC exact `Not enough depth pixels! - low fill factor) Please retry in different lighting conditions` — cohérent avec la suite 64 (exigence de scène OCC, pas un bug).

## État actuel — au 2026-06-18 (Self-cal D405 échoue à 86% fill — exigence OCC, pas un bug)

> **2026-06-18 (suite 64)** : **l'utilisateur corrige mon diagnostic** (« non regarde la dernière capture 86% filled et la calibration ne passe pas ») — sur la capture post-reboot, **Depth fill = 86%** (live stream sain) mais la self-calibration on-chip échoue quand même « Not enough … ». Donc le glare n'explique PAS cet échec-là. **Vraie cause** (vérifiée sur la [doc Intel self-calibration D400](https://dev.realsenseai.com/docs/intel-realsense-self-calibration-for-d400-series-depth-cameras/) + [page produit](https://www.intelrealsense.com/self-calibration-for-depth-cameras/)) : l'OCC tourne sur **son propre profil 256×144@90** (pas le live stream) et échantillonne une région centrale ; le « Depth fill % » affiché à l'écran est donc **sans rapport** avec son succès. L'OCC exige une **surface plane, texturée, remplissant tout le champ**, caméra perpendiculaire, à distance modérée — une petite carte PCB inclinée à ~7cm (limite proche du D405) ne satisfait pas ça → `Fill_Factor_LOW`. **Ce n'est pas un bug applicatif** : c'est un refus attendu du firmware. **Action code** : message « Not enough » réécrit (`RealSenseCapture.cpp`) pour expliquer la vraie exigence (surface plane texturée plein cadre), préciser que c'est indépendant du « Depth fill % » live, et rappeler que l'OCC est **optionnelle** (réduit seulement le bruit de profondeur ; calibration usine valide, Auto-Align/overlay marchent sans). Fichiers : `src/camera/RealSenseCapture.cpp`, docs. [JETSON_ERREURS #39](JETSON_ERREURS.md#erreur-39--distanceauto-alignself-cal-faux-sous-glare-d405) (note suite 64). ⚠️ Non compilé/testé ici.

## État actuel — au 2026-06-18 (Distance/Auto-Align/self-cal faux sous glare D405 — depth fill bas non détecté)

> **2026-06-18 (suite 63)** : **nouveaux tests réels utilisateur sur D405** — deux rapports distincts. (1) Screenshot avec reflet/éblouissement visible sur la carte : « Distance: 104.0mm » alors que la distance réelle est ~70mm, **Depth fill: 11%**, et Auto-Align échoue avec un candidat profondeur de 4702 px² contre 123295 px² attendu (« et maaintenaant il met 100mm alors qu il est à 70 mm »). (2) Après reboot de l'app (« j ai rebooter l apppli mais la calibrtion et auto algin ne fonctionne pas ») : calibration on-chip D405 échoue 3/3 (« Not enough … », tronqué dans l'Event Log) puis « streaming pipeline restored » ; Depth fill remonté à 86%, Distance 72.0mm (cohérent avec le ~70mm réel). **Recherche web** confirme le message complet librealsense : `"Not enough depth pixels! (Fill_Factor_LOW). Please retry in different lighting conditions"` — le firmware applique lui-même un garde-fou sur le taux de pixels profondeur valides, exactement le même concept que le cas (1). **Cause commune** : reflet spéculaire/éblouissement sur la carte (PCB glossy) confond l'appariement stéréo IR du D405 → fill ratio bas → toute valeur dérivée (distance médiane ROI centrale, segmentation de plan `locateViaDepth()`, scale px/mm sténopé) devient fausse-mais-plausible plutôt que d'échouer proprement. **Fix** :
> 1. `BoardLocator.cpp` : constante `kMinDepthFillRatio = 0.20` + garde en tête de `locateViaDepth()` — sous ce seuil, échec explicite (« depth data too sparse… likely glare/reflection… try the contour method ») au lieu de segmenter un plan minuscule/faux.
> 2. `Application.cpp` (handler `depthFrameReady`) : même garde avant le calcul de la médiane de distance — sous le seuil, `setDistance(0.0)` (affiche « — ») et la mise à jour de `m_lastDepthDistanceMm`/scale px/mm est sautée, au lieu d'afficher une valeur fausse mais qui a l'air précise (104.0mm).
> 3. `RealSenseCapture.cpp` : message d'aide spécifique quand le firmware répond « Not enough … » pendant la self-cal on-chip (distinct du message déjà géré « HW not ready », [ERREUR #30](JETSON_ERREURS.md#erreur-30--self-calibration-d405-hw-not-ready)) — explique le lien avec le glare/distance de travail.
> 4. `StatsPanel.cpp` : tooltip avec le message complet sur chaque ligne de l'Event Log — la colonne Message tronque visuellement les messages longs (c'est ce « Not enough … » tronqué qui a motivé ce fix), sans aucun moyen avant ça de voir le texte complet sans aller chercher le fichier log.
>
> Fichiers : `src/overlay/BoardLocator.cpp`, `src/app/Application.cpp`, `src/camera/RealSenseCapture.cpp`, `src/gui/StatsPanel.cpp`. [JETSON_ERREURS #39](JETSON_ERREURS.md#erreur-39--distanceauto-alignself-cal-faux-sous-glare-d405). ⚠️ Non compilé/testé ici. **À valider au prochain build Jetson** : sous glare/reflet, la Distance doit afficher « — » plutôt qu'une valeur fausse, Auto-Align doit échouer proprement avec le message glare au lieu de proposer un mauvais placement, et le message complet de l'échec self-cal doit être lisible au survol dans l'Event Log.
>
> **Par ailleurs, signalé mais pas encore diagnostiqué** : le second screenshot de l'utilisateur (post-reboot, Depth fill 86%, donc cette fois-ci pas un problème de glare) montre un overlay qui semble mal aligné (deux petits îlots jaunes plutôt qu'une couverture homogène de la carte) mais l'Event Log visible à l'écran ne contient que les 4 lignes de self-calibration — la ligne `BoardLocator:`/`Auto-Align …` correspondant à une éventuelle tentative est probablement scrollée au-dessus, hors champ du screenshot. Demandé à l'utilisateur de faire défiler l'Event Log vers le haut (ou de repartager le log terminal complet, qui n'est pas tronqué) pour identifier précisément quel chemin/score Auto-Align a produit cet overlay, avant de pouvoir cibler un fix. Noté en passant (→ [JETSON_ERREURS #40](JETSON_ERREURS.md#erreur-40--settotalcomponents-jamais-appele--inspection-progress-toujours-a-zero), 🔴 OUVERT, pas corrigé cette session) : `StatsPanel::setTotalComponents()` n'est **jamais appelé** nulle part dans le code — le panneau « Inspection Progress » affichera donc toujours « No inspection data » / 0% qu'un iBOM soit chargé ou non, ce qui veut dire qu'on ne peut pas se fier à ce panneau pour savoir si un projet était chargé sur ce screenshot.

## État actuel — au 2026-06-18 (Auto-Align D405 : scale px/mm périmé rejetait le bon contour)

> **2026-06-18 (suite 62)** : **premier test réel Auto-Align sur D405** — échec : `BoardLocator: Depth: candidate quad area (381762 px^2) doesn't match the board outline at the known scale (32771 px^2)`. Ratio ≈ 11.65× (≈3.4× linéaire), largement hors `kAreaTolerance=2.5` → la méthode profondeur avait très probablement trouvé le vrai plan de la carte mais `validateSize()` l'a rejeté car `expectedPixelsPerMm` était calculé depuis une calibration checkerboard potentiellement faite avec une autre caméra/distance, et le live-update par profondeur (`ppmm = fx/distance`) n'est appliqué que si `Config::scaleMethod() == Depth`. **Fix** : nouveau membre `Application::m_lastDepthDistanceMm`, mis à jour sans condition de `scaleMethod()` dans le handler `depthFrameReady` ; `autoAlignBoard()` calcule désormais `expectedPixelsPerMm` en priorité via `fx/distance` (D405) avant de retomber sur le scale caché. Voir [JETSON_ERREURS #38](JETSON_ERREURS.md#erreur-38--auto-align-echoue-sur-d405-scale-pxmm-perime). Fichiers : `src/app/Application.{h,cpp}`. ⚠️ Non compilé ici. **À revalider** : Auto-Align sur D405 doit accepter le contour trouvé par la méthode profondeur sans erreur de taille.

## État actuel — au 2026-06-18 (Fix build Jetson : `tr`/`tl` masquaient `QObject::tr()`)

> **2026-06-18 (suite 61)** : **premier build réel sur Jetson** après le commit de l'audit Auto-Align (#36) — échec : `no match for call to '(const cv::Point_<float>) (const char [38])'` sur l'appel `tr("Auto-Align: aligned via %1...")` dans `autoAlignBoard()`. Cause : le fix #5 de l'audit avait introduit des locales `tl`/`tr` (`cv::Point2f`) dans le même lambda que l'appel `tr(...)` plus bas, masquant `QObject::tr()` pour le reste de la portée. Renommées en `cornerTL`/`cornerTR`. Voir [JETSON_ERREURS #37](JETSON_ERREURS.md#erreur-37--variable-locale-tr-masque-qobjecttr-dans-autoalignboard). Fichier : `src/app/Application.cpp`. Commit `063835e`. **À revalider** : rebuild complet doit passer sans cette erreur.

## État actuel — au 2026-06-18 (README réécrit : Jetson-primary)

> **2026-06-18 (suite 60)** : **utilisateur** (« il va falloir refaire le readme, tant de chose ont changé »). Le `README.md` racine était resté Windows-only et datait d'avant la migration Jetson + l'ajout de nombreux modules. Réécriture complète : (1) plateforme **Jetson AGX Orin (Linux + Docker) = cible active**, Windows déplacé en "legacy" (`windows-legacy`/tag `v0.1.0-windows-final`) avec liens vers les docs JETSON_*. (2) Fonctionnalités mises à jour : RealSense D405 (couleur+profondeur, self-cal, nuage de points), Auto-Align, ancrage microscope 1-point, BoardMinimap, Dataset Creator, moniteur de calibration, fallback MJPG GStreamer CPU, masquage zone carte du tracking ORB. (3) Architecture/threading mis à jour (RealSenseCapture, BoardLocator, DatasetCreator, nouveaux panneaux/dialogues GUI tous listés depuis l'arbo réelle `src/`). (4) Build : section Jetson (bootstrap + run_dev_shell + build_jetson) en premier, Windows déplacé en section "legacy". (5) Structure projet, roadmap, modèles IA (statut "câblé" pour ComponentDetector) actualisés. Tous les liens vérifiés (docs/AI_PIPELINE, DATASET_CREATOR_PLAN, JETSON_MIGRATION, AUTO_ALIGN_PLAN, scripts/*, models/README tous présents). Fichier : `README.md`.

## État actuel — au 2026-06-18 (Auto-Align : audit + correction de bugs post-MVP)

> **2026-06-18 (suite 59)** : **utilisateur a demandé un audit de bugs sur le diff Auto-Align** (« peux tu faire un audit pour trouver des bugs » → « vas y »). Revue multi-angles (code-review skill, plusieurs angles en parallèle) sur `git diff origin/main HEAD` (9 fichiers, 695 lignes — `BoardLocator.{h,cpp}` neufs + `Application.{h,cpp}`, `ControlPanel.{h,cpp}`). **Bugs corrigés** :
> 1. **Projet iBOM périmé dans le callback Auto-Align** — la lambda `finished` lisait `m_ibomProject` (membre live, peut avoir changé/devenir nul pendant la détection sur thread worker) au lieu de la copie `project` capturée au moment du dispatch. Fix : `pcbCorners` construit depuis `project->boardInfo.boardBBox`, `project` ajouté à la capture de la lambda, garde `!m_ibomProject` supprimée (inutile, `project` est non-null par construction).
> 2. **`result.found` toujours vrai dès qu'un candidat existait** — aucun seuil minimal sur le score d'orientation (`scoreOrientation()`), donc un iBOM sans `boardOutline` ni composants sur la couche active (predictedPixels=0 pour les 8 candidats) "gagnait par défaut" avec un score de 0.0 silencieusement reporté comme succès. Fix : constante `kMinAcceptableScore = 0.10` dans `BoardLocator.cpp`, `result.found = !bestImgCorners.empty() && bestScore >= kMinAcceptableScore`, message d'échec distinct ("score too low" vs "no candidates").
> 3. **Filtre de couche manquant dans le scoring d'orientation** — `BoardLocator::scoreOrientation()` rendait *tous* les composants du projet (front+back confondus) alors qu'`OverlayRenderer` ne rend que `m_activeLayer` (`OverlayRenderer.cpp:47`) ; sur une carte double-face cela dilue le score de la bonne orientation avec les empreintes (effet miroir) de l'autre face. Fix : nouveau paramètre `ibom::Layer activeLayer` propagé `locate()` → `disambiguate()` → `scoreOrientation()`, filtre `if (comp.layer != activeLayer) continue;` ajouté (même convention qu'`OverlayRenderer`). `Application::autoAlignBoard()` passe `ibom::Layer::Front` (seule couche actuellement rendue par l'app — pas de toggle front/back côté UI à ce jour).
> 4. **Race entre Auto-Align et un alignement manuel concurrent** — si l'utilisateur déclenchait un alignement manuel (4-points / 2-composants / ancrage) pendant qu'une détection Auto-Align tournait en arrière-plan, le résultat qui arrivait en second écrasait silencieusement l'autre, sans notion d'ordre. Fix : compteur `Application::m_alignmentEpoch` (nouveau membre, `uint64_t`), incrémenté à chaque alignement appliqué (les 5 sites `m_homography->compute(...)` existants + dispatch d'Auto-Align) ; la lambda `finished` capture l'epoch au moment du dispatch et abandonne silencieusement (log info) si l'epoch courant a changé entre-temps.
> 5. **Échelle px/mm pas toujours mise à jour après Auto-Align** — appelait `updateDynamicScale()`, qui peut être un no-op selon `Config::scaleMethod()` (ex: méthode `Depth` sans frame de profondeur). Fix : calcul géométrique direct depuis la nouvelle homographie (même fallback que le handler 4-points manuel : projette le bord haut du board bbox, mesure la largeur en pixels, divise par `bb.width()` en mm), indépendant de `scaleMethod()`.
> 6. **Non corrigé (accepté tel quel)** : la validation taille/aspect (`validateSize()`) est un no-op quand `expectedPixelsPerMm <= 0` (app non calibrée / homographie jamais établie) — le fallback contour perd alors son principal garde-fou anti-faux-positif. Accepté comme limitation connue documentée dans `AUTO_ALIGN_PLAN.md` ("Risks") plutôt que corrigé dans ce passage — le score d'orientation (fix #2) reste un second filet de sécurité dans ce cas.
>
> Fichiers modifiés : `src/overlay/BoardLocator.{h,cpp}`, `src/app/Application.{h,cpp}`. ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV dans ce conteneur) — relecture manuelle uniquement, multi-angles convergentes (les bugs #1 et #2 ont été remontés indépendamment par 2-3 angles différents). **À valider au prochain build Jetson** : Auto-Align sur un iBOM sans `boardOutline`/composants face active doit échouer proprement (pas de "succès" à score 0) ; double-clic Auto-Align + alignement manuel pendant la détection ne doit pas produire d'overlay incohérent ; le panneau Stats doit afficher la bonne échelle après Auto-Align même avec `scaleMethod=Depth` sans profondeur.

## État actuel — au 2026-06-18 (Auto-Align : détection automatique du contour de carte)

> **2026-06-18 (suite 58)** : **nouvelle feature demandée par l'utilisateur** (« pourquoi l'overlay ne s'oriente pas automatiquement pour matcher la carte physique ? » → « prepare un plan pour le faire » → « yes go tu peux tout faire sans me demander »). Implémenté un **6e chemin d'alignement** (« Auto-Align (Beta) ») qui localise le contour de la carte dans l'image caméra courante et calcule l'homographie sans aucun clic, en plus des 5 chemins manuels existants (4-points, 2-composants, ancrage microscope 1-point). Plan complet : [docs/AUTO_ALIGN_PLAN.md](AUTO_ALIGN_PLAN.md). **Approche** : (1) détection du rectangle de la carte par **plan de profondeur** (D405 : médiane de distance sur ROI central ± bande 15mm, isole le plan le plus proche) avec fallback **contour 2D** (Canny + `findContours` + filtre de "quad-likeness" `area/rectArea ≥ 0.55`, marche sur n'importe quelle caméra) ; (2) **validation taille/aspect** contre la taille réelle du board iBOM (rejette les faux contours) ; (3) **désambiguïsation d'orientation** — un rectangle nu a 8 ordres de coins possibles (4 rotations cycliques × 2 sens) ; chacun est scoré en rendant le contour de carte + bboxes composants iBOM à travers l'homographie candidate et en mesurant le recouvrement avec les vraies arêtes Canny détectées dans la frame ; le meilleur score gagne ; (4) **application** identique aux autres chemins : `Homography::compute(pcbCorners, imageCorners)` avec la convention standard `{TL,TR,BR,BL}` de `boardInfo.boardBBox`, puis `OverlayRenderer::setHomography()` + `updateDynamicScale()` + `BoardMinimap::update()` + reset de la référence de tracking live. **Threading** : la détection (Canny/contours/scoring sur 8 candidats, peut prendre des dizaines de ms) tourne sur `QtConcurrent::run` + `QFutureWatcher` (même pattern que `CalibrationMonitorDialog`), hors thread GUI. La frame couleur/profondeur courante est mise en cache en continu (`Application::m_lastColorFrame`/`m_lastDepthFrame`, `shared_ptr<const cv::Mat>` zero-copy déjà existants comme type) dans les lambdas `frameReady`/`depthFrameReady`, puis clonée une fois avant le passage au thread worker. Garde `m_autoAligning` contre les double-clics pendant une détection en cours. **Fichiers** : `src/overlay/BoardLocator.{h,cpp}` (NOUVEAU, Qt-free, testable seul), `CMakeLists.txt` (ajout sources), `src/gui/ControlPanel.{h,cpp}` (bouton + signal `autoAlignRequested`), `src/app/Application.{h,cpp}` (cache frames, handler, `autoAlignBoard()`), `docs/AUTO_ALIGN_PLAN.md` (NOUVEAU). **Écarts volontaires vs plan initial** (communiqués à l'utilisateur) : pas de réutilisation d'`OverlayRenderer::render()` pour le scoring (rendu OpenCV léger dédié dans `BoardLocator`, plus simple et garde la classe Qt-free) ; pas d'option `Config::autoAlignOnLoad` persistée — MVP = bouton manuel seulement. Le chemin détecteur AI (`ComponentDetector`) n'est pas utilisé (`models/` toujours vide) — explicitement hors scope, future amélioration possible. ⚠️ **Non compilé/testé ici** (pas de toolchain Qt6/OpenCV dans ce conteneur) — relecture manuelle uniquement. **À valider au prochain build Jetson** : cliquer "Auto-Align (Beta)" avec une carte chargée + caméra active → l'overlay doit s'orienter sur la carte physique sans clic, message de statut avec méthode (depth/contour) + score.

## État actuel — au 2026-06-18 (ORB tracking : masquage sur la zone carte)

> **2026-06-18 (suite 57)** : **Live Tracking Mode activé mais l'overlay ne suit pas la carte tenue à la main sous une caméra fixe** (screenshot utilisateur : "Homography computed: 4/4 inliers", "Live tracking mode enabled", "TrackingWorker: reference captured (179 keypoints)" — donc tracking actif, alignement initial réussi, mais l'overlay reste figé quand la carte est déplacée). **Cause** : `TrackingWorker::processFrame()` appelait `m_detector->detectAndCompute(small, cv::noArray(), kp, desc)` — **ORB tourne sur l'image entière**, fond compris (table en bois, câbles). Quand l'utilisateur déplace la carte à la main sous une caméra fixe, les points-clés du fond restent statiques alors que seuls ceux de la carte bougent ; `cv::findHomography` (RANSAC) retient le plus grand ensemble cohérent d'inliers, qui peut être le **fond statique** (transform ≈ identité) si le fond a une densité de points-clés comparable ou supérieure à celle de la carte — explique exactement le symptôme observé (overlay figé à sa position d'alignement initiale). **Fix** : masquer la détection ORB à la zone de la carte. Nouveau slot `TrackingWorker::setBoardPolygon(std::vector<cv::Point2f> pcbPoints)` reçoit les 4 coins du bbox de la carte (coords PCB) une fois au chargement d'un iBOM (`Application::loadIBomFile()`, couvre aussi bien l'« Open iBOM » manuel que le rechargement automatique du dernier fichier au démarrage — fonction d'entrée unique confirmée par grep). Nouvelle méthode privée `buildBoardMask(smallSize, downscale)` projette ce polygone via la dernière homographie connue (`m_lastHomography`, mise à jour après chaque estimation réussie dans `processReference()`/`processIncremental()`/`setBaseHomography()`), l'agrandit ×1.6 depuis son centroïde (tolérance au mouvement depuis la dernière estimation), et remplit un masque `CV_8U` (`cv::fillConvexPoly`) passé à `detectAndCompute()`. Sans polygone de carte ou sans estimation d'homographie disponible (juste après un chargement iBOM, avant le premier ancrage), retourne un `Mat` vide → détection non masquée (comportement précédent, pas de régression). Registration cross-thread Qt nécessaire pour le nouveau paramètre `std::vector<cv::Point2f>` : `Q_DECLARE_METATYPE` + `qRegisterMetaType<std::vector<cv::Point2f>>("std::vector<cv::Point2f>")` dans `Application::initialize()` (CLAUDE.md piège #17 — la chaîne doit correspondre exactement au type qualifié du slot). Fichiers : `src/overlay/TrackingWorker.{h,cpp}`, `src/app/Application.cpp`. [JETSON_ERREURS #35](JETSON_ERREURS.md#erreur-35--orb-tracking-verrouille-sur-le-fond-statique-au-lieu-de-la-carte). ⚠️ Non compilé/testé ici (pas de toolchain Qt6/OpenCV dans ce conteneur). **À valider au prochain build Jetson** : déplacer la carte tenue à la main sous une caméra fixe avec Live Tracking Mode actif → l'overlay doit suivre le mouvement dans tous les axes, plus de verrouillage sur le fond.

## État actuel — au 2026-06-17 (Settings → Camera "No camera detected" sur D405 active)

> **2026-06-17 (suite 56)** : **PR #17 mergée sur `main` (squash `8a0d292`)** (self-cal D405 + combo device Application + bbox minimap). Nouveau screenshot utilisateur : tout fonctionne (calibration on-chip réussie, log « RealSense on-chip calibration done, health=0.0280 », « RealSense streaming pipeline restored after calibration ») **mais** le dialogue **Settings → Camera** affiche encore **« No camera detected »** alors que la D405 est manifestement active. **Cause** : `SettingsDialog::enumerateCameras()` a sa propre logique d'énumération RealSense, distincte de celle d'`Application::refreshCameraDeviceList()` corrigée en suite 55/ERREUR #32 — elle n'avait pas le même garde-fou « device busy = device actif, pas absent ». **Fix** : synthétiser `"<index>: Intel RealSense (active)"` quand `realsense && names.isEmpty()` dans le callback de fin d'énumération, comme côté `Application`. Lambda passée à `QMetaObject::invokeMethod` rendue `mutable` (capturait `names`/`indices` en `const` sinon). Fichier : `src/gui/SettingsDialog.cpp`. [JETSON_ERREURS #34](JETSON_ERREURS.md#erreur-34--settings--camera-affiche-no-camera-detected-sur-d405-active). ⚠️ Non compilé/testé ici. **À valider** : Settings → Camera affiche « Intel RealSense (active) » au lieu de « No camera detected » quand la D405 streame déjà.
>
> **Second point soulevé par l'utilisateur (même message)** : sur le screenshot du viewport principal (pas Settings), l'image montre la D405 pointée sur un clavier/table en bois (pas la carte PCB), sans aucun overlay pads/silkscreen visible — juste le crosshair central. **Pas un bug de homographie** : le log confirme une homographie valide (4/4 inliers, 0 px d'erreur) et un ancrage minimap correct. Cause la plus probable, vérifiable directement dans le screenshot du panneau Controls (image 1) : les cases **Show Pads / Show Silkscreen** de la section OVERLAY semblent décochées — dans ce cas l'absence d'overlay sur le clavier est le comportement attendu (rien à corriger côté code), pas un défaut d'alignement. Communiqué à l'utilisateur pour confirmation visuelle des toggles avant d'investiguer plus loin.

## État actuel — au 2026-06-17 (Self-cal D405 profil 256×144@90 + combo device + bbox minimap)

> **2026-06-17 (suite 55)** : **PR #16 mergée sur `main` (squash `cca893a`)** (fallback MJPG GStreamer CPU). **Trois problèmes traités** (screenshot + log utilisateur, sur D405) : (1) **self-calibration D405 `HW not ready`** — l'utilisateur a corrigé mon hypothèse USB2 (« cherche sur le net, n'extrapole pas ») : la D405 est en **USB 3.2**. **Recherche web** → vraie cause : `run_on_chip_calibration()` exige le **flux depth en 256×144 @ 90 fps** au moment de l'appel (contrainte firmware D4xx ; issues librealsense [#7087](https://github.com/IntelRealSense/librealsense/issues/7087)/[#12014](https://github.com/IntelRealSense/librealsense/issues/12014) — marche via le Viewer qui bascule sur ce profil, échoue en script pleine résolution). **Fix** (`RealSenseCapture.cpp`) : stop pipeline → redémarrage depth-only 256×144@90 (lié par serial) → stabilisation → calibration (retry ×3) → **restauration du pipeline normal** dans tous les cas. (2) **Combo Device montrait le microscope sur D405** — `refreshCameraDeviceList()` : énumération RealSense vide (device busy) → garde anti-flicker conservait la liste V4L2. Fix : synthétiser « 0: Intel RealSense (active) » quand backend=RealSense + capture + énum vide ([JETSON_ERREURS #32](JETSON_ERREURS.md)). (3) **Bounding boxes minimap décalées/superposées** — `IBomParser` lisait `bbox.pos` comme coin en ignorant `relpos`/`angle` iBOM. Fix : reconstruire les 4 coins tournés `pos + R(-angle)·(relpos + coin·size)` → vraies bornes AABB ([JETSON_ERREURS #33](JETSON_ERREURS.md)). Fichiers : `src/camera/RealSenseCapture.cpp`, `src/app/Application.cpp`, `src/ibom/IBomParser.cpp` (+`<cmath>`). ⚠️ Non compilé/testé ici. **À valider** : self-cal réussit/échoue proprement + streaming restauré ; combo affiche « Intel RealSense » sur D405 ; bbox composants bien placées sur la minimap.

## État actuel — au 2026-06-17 (Fallback MJPG via pipeline GStreamer CPU)

> **2026-06-17 (suite 54)** : **PR #15 mergée sur `main` (squash `69f7c25`)** (/dev/dri restauré). **Nouveau log + screenshot utilisateur** (build à jour : le warning « streaming YUYV » de la suite 51 apparaît) : le microscope ouvre sur `/dev/video6` en **YUYV 1280×720** → `select() timeout` → pas de feed (« No camera feed »). La HAYEAR **ignore `CAP_PROP_FOURCC`** d'OpenCV même re-posé après la résolution (suite 51 insuffisant). **Fix (suite 54)** : `buildGstPipelineCpu()` + fallback dans `CameraCapture::captureLoop()` — si le FOURCC effectif n'est pas MJPG après ouverture V4L2, ré-ouverture via pipeline GStreamer **CPU** `v4l2src ! image/jpeg ! jpegdec ! videoconvert ! video/x-raw,format=BGR ! appsink`. Les caps `image/jpeg` forcent la négociation MJPG côté driver ; `jpegdec` décode en CPU (**pas d'EGL/NVDEC** → marche headless, contrairement au path HW nvvidconv). Validation par lecture réelle d'une frame avant d'adopter la pipeline (`cap = std::move(gst)`), sinon on garde le flux brut. Fichier : `src/camera/CameraCapture.cpp`. **Deux points communiqués à l'utilisateur** : (a) **device index** — l'app a utilisé `/dev/video6` (config) alors que la HAYEAR est aussi sur `/dev/video0` (combo) ; `video6` = nœud secondaire YUYV-only → sélectionner **Device 0 + Apply Camera** ; (b) **map PCB** — le rendu est en fait **correct/centré** (`BoardMinimap` : carte ~carrée 64×60 dans un dock étroit et haut → grandes marges verticales = comportement normal du `std::min(ww/pcbW, wh/pcbH)`), pas un bug. ⚠️ Non compilé/testé ici. **À valider** : log montre `FOURCC=MJPG` ou « re-opened with CPU MJPG GStreamer pipeline », feed caméra présent sur Device 0. Cf [JETSON_ERREURS #29](JETSON_ERREURS.md).

## État actuel — au 2026-06-17 (Restauration /dev/dri par défaut dans compose.yml)

> **2026-06-17 (suite 53)** : **PR #14 mergée sur `main` (squash `f16e948`)** (fallback caméra par nom + validation pipeline GStreamer). **Investigation du « ça marchait dans les anciennes versions » (HW decode GStreamer)** : trouvé que le commit `64a3b13` (« devices commentés par défaut pour ne pas bloquer le démarrage sans caméra ») avait commenté **tout le bloc `devices:` de `compose.yml`**, y compris **`/dev/dri`** (moteur d'affichage Tegra, nécessaire à EGL pour `nvvidconv`). Or `/dev/dri` est **toujours présent** sur un Jetson démarré et n'aurait pas dû être groupé avec les `/dev/video*` (eux seuls bloquent le démarrage si absents). **Fix** : `/dev/dri` remonté par défaut dans `compose.yml` (services `dev` ET `runtime`), **retiré de `compose.local.yml`** pour éviter le doublon (les listes `devices` sont CONCATÉNÉES par le merge compose → erreur #14). Les `/dev/video*` restent opt-in / générés dynamiquement par les scripts. Fichiers : `docker/compose.yml`, `docker/compose.local.yml`, [JETSON_ERREURS #31](JETSON_ERREURS.md). ⚠️ **Nuance importante communiquée à l'utilisateur** : ses scripts `run_local_gui.sh`/`run_dev_shell.sh` utilisent déjà `compose.local.yml` qui montait `/dev/dri` → pour CE workflow `/dev/dri` était déjà là, donc si `No EGL Display` persiste c'est qu'il manque les nœuds multimédia Tegra (`/dev/nvhost-*`, `/dev/nvmap`, normalement injectés par le nvidia-container-toolkit) — sujet toolkit/L4T. **Recommandation maintenue** : le path CPU MJPG suffit pour le microscope USB 2.0 → laisser le HW decode désactivé sauf besoin. ⚠️ Non testé ici (pas de Docker/Jetson). **À valider** : `docker compose -f docker/compose.yml up dev` nu monte bien `/dev/dri` ; pas de doublon device avec `run_local_gui.sh`.

## État actuel — au 2026-06-17 (Fallback index caméra par nom + validation pipeline GStreamer)

> **2026-06-17 (suite 52)** : **PR #13 mergée sur `main` (squash `d4c997b`)** (MJPG re-forcé + garde-fous self-cal D405). **Deux nouveaux problèmes repérés par l'utilisateur sur logs/screenshot** ([JETSON_ERREURS #31](JETSON_ERREURS.md)) : (1) **microscope inouvrable** — le `config.json` pointe sur `/dev/video6` mais après la ré-énumération USB (collapse du bus, [#28](JETSON_ERREURS.md)) le microscope HAYEAR est passé à **`/dev/video0`** (combo « 0: HAYEAR_CAMERA: MOS-4K Pro — USB 2.0 HS »). (2) **décodage HW GStreamer mort** — activé par l'utilisateur, le pipeline `nvv4l2decoder/nvvidconv` échoue sur « No EGL Display » (pas de display EGL dans le container headless), la négociation des caps s'effondre (`-1x-1 @ 0 fps`), mais `cap.isOpened()` renvoie `true` → le code acceptait ce pipeline mort et sautait le fallback CPU. **Fix** (`src/camera/CameraCapture.cpp`) : (a) **fallback par nom** — si l'index configuré échoue, scan `listDevices()` → ouvre la 1re caméra capture non-RealSense, adopte son index réel (`m_deviceIndex = idx`) ; (b) **validation du pipeline GStreamer** — lit réellement une frame (10 × 50 ms) avant de faire confiance à `isOpened()` ; si rien ne sort → `release()` + fallback CPU V4L2. **Recommandation à l'utilisateur** : laisser le HW decode **désactivé** sur ce setup (le path CPU MJPG suffit pour un microscope USB 2.0 ; EGL dans le container = sujet Docker à part). ⚠️ **Non compilé ni testé ici** (pas de toolchain). **À valider au prochain build** : le microscope s'ouvre même avec un vieil index en config ; HW decode activé retombe proprement sur CPU.

## État actuel — au 2026-06-17 (Forçage MJPG V4L2 + garde-fous self-calibration D405)

> **2026-06-17 (suite 51)** : **PR #12 mergée sur `main` (squash `bf1f1f5`)** (fix freeze + layout Calibration Monitor + affichage type USB par caméra — réaligné via rebase sur `main`, les commits déjà squash-mergés #10/#11 ayant été détectés et sautés). **Deux nouveaux problèmes traités** (feu vert utilisateur « go pour tout avec les garde-fous ») : (1) **MJPG V4L2 ne « collait » pas** — le log Jetson montrait le microscope HAYEAR en **YUYV 1280×720** malgré le `cap.set(CAP_PROP_FOURCC, MJPG)` déjà présent (ligne ~318). Cause classique : plusieurs drivers UVC / le backend V4L2 d'OpenCV **resettent le pixelformat en YUYV quand on change la résolution**, annulant le FOURCC posé avant. **Fix** : re-poser MJPG **après** `CAP_PROP_FRAME_WIDTH/HEIGHT/FPS` dans `CameraCapture::captureLoop()`, + un **warning spdlog explicite** si le FOURCC effectif n'est toujours pas `MJPG` (signale la pression bande passante USB / fps réduits). (2) **Self-calibration D405 échouait** avec `hwmon command 0x80(...) failed (response -7= HW not ready)` — erreur **firmware** (pas applicative) renvoyée par `run_on_chip_calibration()` quand le D4xx n'est pas dans un état stable (lancée trop tôt après un switch de backend, ou lien USB2 dégradé). **Garde-fous ajoutés** : côté capture (`RealSenseCapture.cpp`) — refus si depth désactivé / aucun flux (`colorFps`/`depthFps` ≤ 0), warning si lien USB 2.x, **retry x3 avec pause 800 ms** sur l'erreur transitoire, message d'aide enrichi (USB3 + immobile + surface texturée) ; côté UI (`RealSenseControlsDialog.cpp`) — pré-checks avant d'envoyer la requête (caméra qui diffuse + depth actif) avec QMessageBox d'avertissement clair, et mention USB3 dans la confirmation. Fichiers : `src/camera/CameraCapture.cpp`, `src/camera/RealSenseCapture.cpp`, `src/gui/RealSenseControlsDialog.cpp`. ⚠️ **Non compilé ni testé ici** (pas de toolchain) — relecture manuelle. **À valider au prochain build Jetson** : (a) le log microscope affiche `FOURCC=MJPG` (plus de `select() timeout`, fps stable) ; (b) la self-cal D405 réussit (ou échoue avec un message clair) quand la caméra a eu le temps de se stabiliser sur USB3. **Reste matériel/non corrigeable en soft** : le collapse USB (reset xHCI Tegra) au switch de backend — le MJPG devrait réduire la pression mais ne garantit pas l'absence de reset (cf [JETSON_ERREURS #29](JETSON_ERREURS.md)).

## État actuel — au 2026-06-17 (Affichage type USB + atténuation freeze switch caméra)

> **2026-06-17 (suite 50)** : **diagnostic d'un freeze au switch caméra + disparition de tout l'USB sur le Jetson** (logs réels fournis : microscope V4L2 ouvre en YUYV 1280×720, `select() timeout` répétés → aucune frame ; au passage vers la D405 l'appli freeze ; `lsusb` ne montre plus que les root hubs). **Trois problèmes** identifiés (cf [JETSON_ERREURS #28](JETSON_ERREURS.md) et [#29](JETSON_ERREURS.md)) : (1) **microscope sans frame** = YUYV (non MJPG) à 720p ≈ 442 Mbit/s sature l'USB 2.0 (le microscope est USB 2.0, même sur le hub 3.2 Gen1 de l'utilisateur) ; (2) **freeze GUI** = `switchCameraBackend()` appelait `refreshCameraDeviceList()` → `RealSenseCapture::listDevices()` → `rs2::context::query_devices()` **synchrone sur le thread GUI** pendant le hot-swap, doublé par l'énumération du thread de capture ; (3) **disparition USB** = reset du contrôleur xHCI Tegra (matériel/kernel, non corrigeable en logiciel ; un hub alimenté règle la puissance mais pas forcément le reset de lien). **Feature demandée par l'utilisateur + implémentée** : **affichage du type de lien USB par caméra**. `CameraCapture::listDevices()` (Linux) lit la vitesse négociée dans sysfs (`/sys/class/video4linux/videoN/device` → remonte au nœud USB → fichier `speed` en Mbit/s) et l'ajoute au nom (« HAYEAR_CAMERA — USB 2.0 HS (480 Mb/s) ») ; `RealSenseCapture::listDevices()` ajoute `RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR` (« — USB 3.2 »). Le combo Device montre désormais directement pourquoi le microscope sature. **Atténuation du freeze** : `switchCameraBackend()` diffère l'énumération via `QTimer::singleShot(1500, …)` (hors de l'instant fragile, une seule énumération) ; `refreshCameraDeviceList()` ne bascule plus le combo en « No camera detected » si l'énumération revient vide **alors qu'une caméra capture** (cas RealSense occupée). Fichiers : `src/camera/CameraCapture.cpp` (helper `usbLinkTag()` + includes `<filesystem>`/`<fstream>`), `src/camera/RealSenseCapture.cpp`, `src/app/Application.cpp`. ⚠️ **Non compilé ni testé ici** (pas de toolchain) — relecture manuelle. **À valider au prochain build Jetson** : (a) le combo affiche le type USB des deux caméras ; (b) le switch ne freeze plus ; (c) capturer `dmesg` si l'USB retombe pour confirmer le reset xHCI (le collapse USB reste un sujet matériel). Note : `std::filesystem` ne nécessite pas `-lstdc++fs` sur GCC 11 (Jammy).

## État actuel — au 2026-06-17 (Fix freeze + layout Calibration Monitor)

> **2026-06-17 (suite 49)** : **PR #10 mergée sur `main` (squash `3e818fb`)**, puis fix warning `-Wtype-limits` (`IBomParser::decompressLZString` : `c < 256` toujours vrai car `c` est `unsigned char` → retiré, PR #11 squash `7a6ad5e`). **Utilisateur a testé le Calibration Monitor en live sur le Jetson (screenshot) : freeze de l'appli + layout cassé** (libellés tronqués « ✗ not », « 265 ( », « mean » coupés ; fenêtre visiblement plus étroite que prévu). **Diagnostic** : (1) **freeze** — `onFrame()` appelait `refreshDetection()` **directement sur le thread GUI** depuis le lambda `frameReady`, et `cv::findChessboardCornersSB` (flag `CALIB_CB_ACCURACY`) peut prendre largement plus que les 180 ms de throttle pour conclure à un échec sur une frame sans damier (essaie plusieurs seuils) → bloque toute la boucle d'événements Qt pendant que le dialogue est ouvert. La D405 (848×480) est en plus **sous** `kDetectMaxWidth` (1024) donc tournait sans downscale, aggravant le coût. (2) **layout** : pas de cause certaine identifiée à distance (pas de capacité de repro), mais `setMinimumWidth(560)` sans `resize()` explicite + labels sans `setWordWrap` sur le `QFormLayout` de gauche pouvaient se faire écraser par certains gestionnaires de fenêtres. **Fix** : détection déplacée sur un thread `QtConcurrent::run` (déjà lié via `Qt6::Concurrent` dans `CMakeLists.txt`) — `computeDetection()` (fonction libre, pure, sans accès widget) tourne en arrière-plan, résultat livré via `QFutureWatcher<CalibDetectionResult>::finished` → `onDetectionFinished()` applique le résultat aux labels/preview sur le thread GUI ; garde `m_detectionBusy` qui **droppe** les frames tant qu'une passe est en cours (pas d'empilement). `onFrame()` ne fait plus que cloner + lancer le calcul, retour quasi instantané → la frame suivante de la boucle Qt n'est jamais bloquée. Layout : `setMinimumSize(720, 640)` + `resize(820, 720)` explicite à la construction, `m_lblDetect/m_lblSharp/m_lblCoverage/m_lblBright` passés en `setWordWrap(true)`, `QFormLayout::ExpandingFieldsGrow` + largeur mini 320px sur le groupe « Live frame ». Fichiers : `src/gui/CalibrationMonitorDialog.{h,cpp}`. ⚠️ **Non compilé ni re-testé ici** (pas de toolchain Qt6/OpenCV dans ce conteneur) — relecture manuelle uniquement. **À valider au prochain test Jetson** : ouvrir Dev → Calibration Monitor en live, vérifier que l'appli ne freeze plus (notamment quand le damier n'est PAS dans le champ) et que le layout s'affiche correctement (rien de tronqué).

## État actuel — au 2026-06-17 (Dev : Calibration Monitor live + bouton Copier)

> **2026-06-17 (suite 48)** : **PR #9 mergée sur `main` (squash `5230d09`)** puis branche `claude/pensive-euler-pvde0v` réalignée sur `origin/main` pour repartir propre. **Nouvelle feature demandée par l'utilisateur** : un pop-up dans le menu **Dev** qui montre en **live** tout ce qu'il faut pour la calibration, plus les logs anormaux, et un **bouton « Copier »** pour me recoller les infos. Implémenté `gui/CalibrationMonitorDialog.{h,cpp}` (non-modal, raccourci **Ctrl+Shift+C**, entrée « Calibration Monitor (live)… » dans le menu Dev). Contenu : (1) **bannière de verdict** en haut (Waiting / ✗ damier non détecté / détecté mais FLOU / trop petit / **READY ✓**) ; (2) **aperçu live** (QLabel pixmap, 360px) avec les coins du damier dessinés (`drawChessboardCorners`) ; (3) **détection live** du damier (`findChessboardCornersSB` puis fallback `findChessboardCorners`, sur une copie downscalée ≤1024px, throttle 180 ms) → trouvé ?/coins trouvés vs attendus/méthode ; (4) **netteté** (variance Laplacien sur copie 0.25× = même métrique que le focus-assist, comparée à `datasetMinSharpness`) ; (5) **couverture** (% du cadre + quadrant du centroïde → incite à varier la pose entre les 5 prises) ; (6) **luminosité** moyenne + alerte trop sombre/trop clair (glare bague polarisée) ; (7) **paramètres damier** (cols×rows, mm, coins attendus) lus en live depuis `Config` ; (8) **état calibration** (backend, résolution réelle, RMS, px/mm, FOV, chemin `calibration.yml`) poussé par `Application::pushCalibrationMonitorState()` (appelé depuis `updateCalibrationUI()` → couvre switch backend + fin de calibration) ; (9) **progression capture** N/5 poussée depuis le `calibHandler` ; (10) **journal des problèmes** live (WARN/ERR/CRIT via `LogBridge::messageLogged`, rolling 300 lignes, coloré) ; (11) bouton **« Copier le rapport »** → assemble un texte complet (état + métriques live + logs récents + chemin du fichier log) dans le presse-papier ; (12) bouton **« Capture image »** qui pilote le flux de capture normal (signal→signal `captureRequested`→`MainWindow::calibrationRequested`→`calibHandler`). Câblage : frames poussées dans le lambda `frameReady` **uniquement quand le dialogue est visible** (`onFrame` throttle en interne). Le dialogue est créé paresseusement (1er ouverture) puis gardé en vie (continue de bufferiser les warnings). Fichiers : `CMakeLists.txt` (+ .cpp/.h), `MainWindow.{h,cpp}` (action + signal `calibrationMonitorRequested`), `Application.{h,cpp}` (membre `m_calibMonitor`, includes, wiring, `pushCalibrationMonitorState()`). ⚠️ **Non compilé ici** (pas de toolchain Qt6/OpenCV dans ce conteneur) — relecture manuelle uniquement. **À valider au prochain build Jetson** : ouvrir Dev → Calibration Monitor pendant la capture caméra, vérifier la détection live + le rapport copié.

## État actuel — au 2026-06-16 (PR #9 : fixes des 7 commentaires Copilot)

> **2026-06-16 (suite 47)** : **PR #9 ouverte sur `main`, abonnement aux events, revue automatique Copilot reçue (8 commentaires sur 25 fichiers)**. Triage et correction des 7 commentaires actionnables (le 8e, suggestion d'ajouter des tests unitaires pour le mode incrémental, noté mais non traité ce tour — pas un bug, du nice-to-have) : (1) **`TrackingWorker::resetReference()`** assignait `m_state` directement au lieu de passer par `setState()` → un retour Locked→Lost (re-anchor, changement de base homography) n'émettait pas `trackingStateChanged`, laissant le message de statut UI périmé. Fix : `setState(State::Lost)`. (2) **Bootstrap incrémental** émettait `homographyUpdated(H, kp.size(), 0.0)` en utilisant le **nombre de keypoints comme `inliers`** — alors qu'aucun match/RANSAC n'a encore eu lieu à ce stade ; `DatasetCreator` filtre sur `min_inliers`, donc une session dataset pouvait passer la porte qualité juste après un (re-)ancrage sans vraie mesure. Fix : ne plus émettre `homographyUpdated` au bootstrap (l'overlay a déjà la bonne homographie via l'action d'ancrage elle-même ; la frame suivante émet une vraie mesure). (3) **`SettingsDialog::enumerateCameras()`** lançait un `std::thread` détaché capturant `this`, avec un `QMetaObject::invokeMethod(this, ...)` différé — use-after-free si le dialogue est fermé avant la fin du scan. Fix : `QPointer<SettingsDialog> guard` capturé par valeur (thread-safe à tester, pas à déréférencer hors thread GUI), vérifié avant `invokeMethod` et dans la lambda postée. (4) **`Application::switchProfile()`** restaure homographie/échelle pour le profil entrant mais force `m_liveMode=false` → le handler normal `homographyUpdated` ne tourne pas → `OverlayRenderer`/`StatsPanel`/`BoardMinimap` gardaient les valeurs de l'ancien profil. Fix : push explicite de l'état restauré (`m_overlayRenderer->setHomography()`, `sp->setScale()`, `boardMinimap()->update()`) juste après la restauration. (5) **`BoardMinimap`** initialisée avec la résolution **nominale du config** (`cameraWidth/Height`), pas la résolution réelle de la caméra active (ex. D405 par défaut à 848×480 alors que le config générique est 1920×1080) → rectangle FOV faux jusqu'au prochain resize. Fix : dans le handler `frameReady`, un flag one-shot `minimapSized` (même pattern que `intrinsicsShown`) repousse `setHomography(hom, QSize(frame.cols, frame.rows))` avec la vraie taille dès la première frame du backend actif. (6) **`BoardMinimap::rebuildCache()`** calculait `scale = min(ww/pcbW, wh/pcbH)` sans garde si `pcbW`/`pcbH` == 0 (boardBBox ou bbox composants dégénérée) → inf/NaN cassant le rendu et le mapping clic→PCB. Fix : clamp `pcbW`/`pcbH` à `1.0` si < 1e-6. (7) **Doc périmée** : `MICROSCOPE_PLACEMENT_PLAN.md` affichait toujours « Statut : Planification — non implémenté » alors que cette PR implémente profils caméra/minimap/ancrage/tracking incrémental → statut corrigé en « Partiellement implémenté ». ⚠️ Aucun de ces fixes n'a pu être compilé dans ce conteneur (pas de toolchain Qt6/OpenCV ici) — relecture manuelle attentive des diffs uniquement. **À valider au prochain build Jetson + test manuel** (switch de profil, anchor incrémental, fermeture rapide de SettingsDialog pendant un scan caméra).

## État actuel — au 2026-06-16 (Gate qualité calibration : RMS borné avant sauvegarde)

> **2026-06-16 (suite 46)** : **gate qualité calibration** (l'utilisateur a obtenu une calibration microscope à **RMS 11.09 px** — vs 0.876 avant — acceptée, sauvegardée et appliquée → overlay cassé, bonne calibration écrasée). `runCalibration()` ne testait que `error < 0` (damier non détecté) et acceptait n'importe quel RMS. **Fix** : si `error > 1.5 px`, QMessageBox d'avertissement (défaut No) ; si non forcé → pas de save/apply, et rollback via `m_calibration->load(calibPath)` + `initUndistortMaps` pour restaurer la calibration précédente. `pixels/mm` quasi identique (11.56) confirme que la détection de coins allait, mais le bundle adjustment divergeait — cause = champ étroit du microscope (peu de variation de pose entre les 5 prises) + éventuel flou. Message d'aide : incliner le damier à un angle différent à chaque prise, rester net. Cf [JETSON_ERREURS #27](JETSON_ERREURS.md). ⚠️ Non re-testé. **À diagnostiquer (pas encore corrigé)** : l'utilisateur signale « l'image D405 saute de temps en temps comme si elle essayait de faire qqch » à 7 cm — on-chip self-calibration écartée (bouton only, pas périodique) → pistes : auto-exposure qui chasse au près (surface brillante + bague polarisée), ou calcul périodique du nuage 3D (~1 Hz) si la vue 3D/Depth est active. Question de clarification posée à l'utilisateur.

## État actuel — au 2026-06-16 (Fix stats : D405 fx=0 + Distance/Depth fill périmés sur microscope)

> **2026-06-16 (suite 45)** : **2 bugs d'affichage du panneau Statistics repérés par l'utilisateur sur screenshots** (calibration microscope réussie entre-temps : checkerboard 5 images, RMS 0.876, **11.58 px/mm** brut — l'autorité de mesure réelle). (1) **D405 « Factory fx=0.0 px »** alors que les logs montrent `fx=436.8px` : `updateCalibrationUI()` est appelée synchroniquement juste après `start()`, mais `RealSenseCapture` met en cache les intrinsics sur le thread de capture lors de la 1re frame → lecture trop tôt, `colorFx()=0`. **Fix** : refresh one-shot dans le handler `frameReady` (flag `intrinsicsShown` en init-capture `mutable`, donc auto-reset par connexion à chaque hot-swap) dès que `backend==RealSense && colorFx()>0`. Cf [JETSON_ERREURS #24](JETSON_ERREURS.md). (2) **Microscope : « Distance: 190.0 mm » + « Depth fill: 77% » périmés** (valeurs D405 jamais réinitialisées au switch — ces labels ne sont alimentés que par `depthFrameReady`, qui ne se déclenche jamais en V4L2). **Fix** : branche V4L2 de `updateCalibrationUI()` appelle `setDistance(-1)`/`setFillRate(-1)` → « — ». Cf [JETSON_ERREURS #25](JETSON_ERREURS.md). ⚠️ Non re-testés. (3) **Scale microscope 3.5 px/mm = double comptage optique** (corrigé) : `opticalMultiplier` (0.3 dans le profil Microscope) était multiplié par-dessus le résultat de la calibration checkerboard (11.58 px/mm), donnant 3.47 — physiquement faux. L'utilisateur a précisé son optique réelle (**0.35× sous la caméra + 0.7× sous la lentille du microscope**, + une bague polarisée pour l'éclairage, sans effet sur l'échelle). Point clé expliqué à l'utilisateur : **la calibration mesure déjà l'échelle à travers TOUTE la chaîne optique** — inutile de saisir 0.35 ou 0.7 nulle part. **Fix (calibration autoritaire)** : (a) calibration terminée → `m_currentPixelsPerMm = m_basePixelsPerMm` (plus de `× opticalMultiplier`) ; (b) handler settings-apply → multiplier appliqué **uniquement si non calibré** ; (c) défaut profil Microscope `opticalMultiplier` 0.3 → 1.0. Les chemins homography/depth fixaient déjà l'échelle sans multiplier → cohérent désormais. Cf [JETSON_ERREURS #26](JETSON_ERREURS.md). ⚠️ Le `config.json` existant de l'utilisateur garde `optical_multiplier: 0.3` mais c'est désormais inoffensif tant que la caméra est calibrée (multiplier ignoré). (4) **Mesures matériel D405 ⇄ Microscope enregistrées** dans `MICROSCOPE_PLACEMENT_PLAN.md` §9 (les deux screenshots utilisateur montraient la **différence de zoom sur la même carte**) : D405 ≈ 2.3 px/mm @188 mm → FOV ≈ 366×207 mm ; microscope (au zoom calibré) = 11.58 px/mm → FOV ≈ 166×93 mm ; ratio ≈ 5×. Caveat : zoom continu → ces chiffres ne valent qu'au zoom de calibration. Conclusion : même calibré, un 0201 ne fait que ~7 px → il faut zoomer plus (cible ~80–130 px/mm, FOV ~15–25 mm, 1–5 composants visibles) → confirme la priorité du matching incrémental §2. Q5 (index V4L2) passé à 🟡 partiellement résolu (caméra = `/dev/video6` HAYEAR_CAMERA, énumérée par nom). Précision optique notée : **0.35× adaptateur caméra + 0.7× lentille micro** (le `0.3` n'était que l'ancienne valeur du multiplier, désormais à 1.0).

## État actuel — au 2026-06-16 (Fix combo Microscope listait aussi les nodes UVC du D405)

> **2026-06-16 (suite 44)** : **confirmation utilisateur du fix ERREUR 22** (screenshots + logs : device 6 = HAYEAR_CAMERA s'ouvre correctement, calibration checkerboard 5 images réussie, RMS 0.876, **11.58 px/mm** mesuré — bien plus fiable que le 3.47 px/mm affiché précédemment par `FovMeasureDialog` qui lisait une homographie pas encore calibrée). **Nouveau bug repéré par l'utilisateur sur un 3e screenshot** : en profil Microscope, le combo Device liste **aussi** les 3 nodes RealSense (« 0/2/4: Intel(R) RealSense(TM) Depth… ») en plus du microscope (« 6: HAYEAR_CAMERA… »), alors que ce combo ne devrait montrer que les caméras UVC génériques. Cause : le D405 expose ses flux couleur/IR/depth comme de vrais nodes `/dev/video*` UVC en plus de l'API RealSense SDK — `VIDIOC_QUERYCAP` (ajouté en ERREUR 22) les rapporte donc comme caméras capture valides, sans distinction. Sélectionner une de ces entrées en mode V4L2 ouvrirait un flux RealSense brut non rectifié, sans rapport avec le pipeline RealSense SDK normal — trompeur/cassé. **Fix** : `CameraCapture::listDevices()` filtre désormais (`continue`) tout node dont le nom de carte contient `"RealSense"`. Cf [JETSON_ERREURS #23](JETSON_ERREURS.md). ⚠️ Non re-testé — à valider au prochain build : le combo Microscope ne doit plus lister que `6: HAYEAR_CAMERA…`.

## État actuel — au 2026-06-16 (Fix index device V4L2 réel + FovMeasureDialog D405 + crash switch caméra)

> **2026-06-16 (suite 43)** : **fix majeur — microscope inatteignable via l'UI** (logs Jetson : « can't open camera by index » sur tous les `/dev/video`, mais `video6` absent des échecs = seul node ouvrable). Cause = triple confusion **position du combo ↔ index `/dev/video` réel** : `listDevices()` renvoyait `["Camera 6"]` (index perdu en positionnel), les combos étiquetaient/sélectionnaient par position, et `cameraIndex()`/`accept()` renvoyaient `currentIndex()` (position) comme index device → sélectionner l'unique caméra appelait `setDeviceIndex(0)` → `/dev/video0` (node D405/non-capture) → échec. Le microscope (video6) était littéralement inatteignable. **Fix** : (1) `CameraCapture::listDevices()` → `vector<pair<int,string>>` (index réel + nom), énumération Linux via `::open`+`VIDIOC_QUERYCAP` (donne l'index réel, le **nom de carte** pour distinguer microscope/D405, filtre les nodes non-capture via `V4L2_CAP_VIDEO_CAPTURE`, **supprime le spam de warnings OpenCV** GStreamer/obsensor) ; (2) combos stockent l'index réel en `itemData`, `ControlPanel::setCameraDevices(labels, indices, current)` + sélection par `findData` ; `cameraIndex()` et `SettingsDialog::accept()` lisent `currentData()`. RealSense inchangé (index positionnel correct). Cf [JETSON_ERREURS #22](JETSON_ERREURS.md). ⚠️ Non re-testé — au prochain build, le menu déroulant Device doit montrer le nom réel de la caméra (ex. « 6: HD USB Camera ») ; la sélectionner doit ouvrir le bon node. **Action utilisateur** : sur le Jetson, `v4l2-ctl --list-devices` confirme quel node est le microscope.

## État actuel — au 2026-06-16 (Fix FovMeasureDialog D405 + crash switch caméra + Dev menu)

> **2026-06-16 (suite 42)** : **correction FovMeasureDialog pour la D405 (focale fixe, pas de zoom)**. L'utilisateur a rappelé que **la D405 n'a aucun grossissement** — son échelle (~2.31 px/mm mesurée à 190 mm) vient de `fx / distance` (depth) et ne varie qu'avec la distance de travail, pas un zoom. Le zoom continu / fort grossissement = **microscope uniquement**. Le dialogue affichait à tort « homography (live) » comme source d'échelle pour la D405 et recommandait de régler `anchor_pixels_per_mm` (réglage microscope, sans objet pour la D405). Fix : (1) `Metrics::isMicroscope` (= backend V4L2) ; (2) source d'échelle correcte — « depth (fx / working distance) » si D405+ScaleMethod::Depth, sinon « homography (live) » ; (3) section recommandations branchée : D405 → message « pas de zoom, échelle depth live, rien à configurer, bascule sur le profil Microscope pour régler » ; microscope → recommandation anchor_pixels_per_mm (avec note « bootstrap seulement, l'homographie live raffine »). Aucun changement de comportement caméra, juste l'affichage Dev. **2 défauts d'affichage corrigés** vus sur screenshot utilisateur : (a) titre QGroupBox « Scale & Field of View » → le `&` était pris comme mnémonique (rendu « Scale _Field… ») → échappé en `&&` ; (b) ligne Calibration affichait « NOT loaded — undistort disabled » en orange pour la D405 (trompeur : la D405 est calibrée usine, streams rectifiés par le SDK) → affiche « Factory-calibrated (RealSense intrinsics) » sans alerte pour le backend D405. ⚠️ Non re-testé.

## État actuel — au 2026-06-16 (Fix crash switch caméra + spam log + Dev menu)

> **2026-06-16 (suite 41)** : **fix crash SIGABRT au switch caméra/profil + spam log scale** (logs Jetson réels fournis). (1) **Crash** (`terminate called without an active exception` → SIGABRT, trace via `QComboBox::currentIndexChanged`) : quand un device V4L2 échoue à l'ouverture, `captureLoop()` s'auto-termine en mettant `m_capturing=false`, laissant un `std::thread` **joignable** (fini, jamais joint). `CameraCapture::stop()` faisait `if (!m_capturing.load()) return;` **avant** le join → au `m_camera.reset()` (switch profil) ou au destructeur, le `unique_ptr<std::thread>` détruisait un thread joignable → `std::terminate()`. Fix : `stop()` ne early-return plus, joint toujours le thread (via `m_capturing.exchange(false)`, même pattern que `RealSenseCapture::stop()` déjà corrigé) ; `start()` (les deux backends) joint/reset un thread résiduel avant de réassigner `m_thread`. Cf [JETSON_ERREURS #21](JETSON_ERREURS.md). (2) **Spam log** : `Measurement::setCalibration()` loggait en `info` à chaque frame (« calibration set to 2.31 px/mm » en boucle, inondait l'Event Log GUI). Fix : early-return si Δ < 0.05 px/mm + passage en `debug`. (3) **measure_fov.py** : message d'erreur pyrealsense2 amélioré (3 options : build bindings / node UVC du D405 en V4L2 / dialogue Dev in-app). ⚠️ Crash + spam corrigés mais **non re-testés sur Jetson** — à valider au prochain build. **Mesure observée** : scale microscope ~2.31 px/mm (très faible → FOV large, faible grossissement au moment du test).

## État actuel — au 2026-06-16 (Dev menu + measure_fov.py + suivi incrémental + BoardMinimap)

> **2026-06-16 (suite 40)** : **Menu Dev + outil de mesure FOV/scale**. Ajout d'un menu « Dev » dans la barre de menus (entre View et Help), avec l'action « Measure FOV & Scale… » (`Ctrl+Shift+M`). Ouvre `FovMeasureDialog` : dialogue read-only qui calcule et affiche (1) profil actif, backend, résolution, état calibration (avec RMS), (2) px/mm courant (source : homographie live ou fallback config), FOV en mm (largeur × hauteur), (3) nombre de composants iBOM dans le champ courant (requête spatiale : coins image → `imageToPcb()` → bbox → intersection), (4) recommandations config : `anchor_pixels_per_mm` suggéré vs valeur stockée, alerte si différence > 0.5 px/mm, (5) commande exacte pour `scripts/measure_fov.py`. Script Python `scripts/measure_fov.py` : capture N frames (V4L2 ou RealSense), détecte un damier pour calculer px/mm (undistort si calibration.yml fourni), en fallback pour la D405 utilise la depth médiane + HFOV 87° pour estimer la FOV sans damier, sort un rapport JSON + résumé stdout + `config_recommendations` (anchor_pixels_per_mm, reanchor_drift_px, incremental). Deux caméras supportées par `--camera v4l2|realsense`. `FovMeasureDialog.{h,cpp}` ajoutés au CMake. ⚠️ Non testé — à utiliser sur Jetson pour mesurer Q2/Q3 du plan microscope.

## État actuel — au 2026-06-16 (Étape 2 suivi incrémental + BoardMinimap + ancrage 1-point)

> **2026-06-16 (suite 39)** : **Étape 2 plan microscope — suivi incrémental (frame→frame)**. Le `TrackingWorker` faisait du matching **référence fixe** : chaque frame matchée contre une unique frame de référence → à fort grossissement (champ étroit), dès que le microscope s'éloigne de la vue de référence, l'overlap s'effondre et le tracking lâche. Ajout d'un **mode incrémental** : chaque frame est matchée contre la **frame précédente** (overlap toujours élevé), les deltas d'homographie sont **composés** (`m_cumulativeH = deltaH * m_cumulativeH`, PCB→image courante), amorcé à l'ancrage (`m_baseHomography`). Coût = **drift** cumulé, borné par re-ancrage. Implémenté dans `TrackingWorker.{h,cpp}` : `enum class State {Locked, Drifting, Lost}` ; slot `setIncrementalMode(bool, double driftThresholdPx)` ; signal `trackingStateChanged(int)`. `processFrame()` refactorisé : prep commune (gray/downscale/detect/rescale) puis dispatch `processReference()` (comportement d'origine inchangé) ou `processIncremental()`. Helpers extraits `matchPoints()` (knn+Lowe) et `medianReprojError()` (réutilisés par les 2 modes). Drift = Σ erreur reproj médiane par frame (proxy conservateur, flag tôt) ; >seuil → Drifting ; ≥4 frames sans match → Lost. `resetReference()` réinitialise drift+cumulative+state (donc re-ancrage = reset). **Câblage Application** : `setIncrementalMode` appelé à l'init, au changement de réglages, et à `switchProfile` — **gated sur backend V4L2** (microscope) ET flag config : la D405 (RealSense) garde le matching global, conforme §0bis du plan. Signal `trackingStateChanged` → badge texte dans la barre d'état (« Tracking: locked / drifting — re-anchor (A) / LOST »). **Config** : `microscope.incremental` (bool, défaut false) + `microscope.reanchor_drift_px` (double, défaut 40). **SettingsDialog** onglet Tracking : checkbox « Incremental frame→frame tracking » + spin « Re-anchor drift ». ⚠️ Non testé — valider sur Jetson avec le microscope (mesurer Q2/Q3 du plan : champ réel + nb composants visibles). Reste : Étape 3 (échelle live lissée depuis homographie) et Étape 4 (IA 0201).

## État actuel — au 2026-06-16 (BoardMinimap + ancrage 1-point microscope + profils caméra)

> **2026-06-16 (suite 38)** : **BoardMinimap** — nouveau widget `src/gui/BoardMinimap.{h,cpp}` (dock gauche, onglet « PCB Map » avec Inspection/Dataset). Vue miniature du PCB entier en coordonnées PCB (mm), sans image caméra. Rendu : fond sombre, outline du PCB (boardBBox), un rect par composant (sélectionné=jaune, placé=vert atténué, normal=bleu accent). Rectangle FOV cyan (tirets) : les 4 coins image → `Homography::imageToPcb()` → pcbToWidget, mis à jour à chaque tracking update + chaque ancrage. Clic sur la minimap → `anchorRequested(cv::Point2f pcbPt)` → `Application` calcule une homographie de similarité centrée sur ce point (même math que l'ancrage 1-point, mais imgCenter comme cible au lieu d'un clic image) → repositionne l'overlay en direct. `BoardMinimap::setHomography(Homography*, QSize)` reçoit le pointeur stable une fois dans `initialize()` puis lit l'état courant à chaque `paintEvent`. Mises à jour `update()` : à chaque `homographyUpdated` (tracking worker), à chaque ancrage 1-point réussi, et à chaque minimap-anchor réussi. `setIBomData` à chaque chargement iBOM, `setSelectedRef` à chaque sélection BOM, `setPlacedRefs` à chaque `stepPlaced` et à la restauration de session. Ajouté à `CMakeLists.txt` (sources + headers). ⚠️ Non testé — valider sur Jetson.

> **2026-06-16 (suite 37)** : **Étape 1 plan microscope — ancrage 1-point**. Pour le placement à fort grossissement où le tracking ORB global ne verrouille pas (champ étroit), ancrage manuel : sélectionner un composant dans le BOM → bouton « Anchor on Component » (toolbar, raccourci **A**) → cliquer le composant dans l'image → homographie de similarité construite à partir de cette **seule** correspondance, en réutilisant l'échelle live `m_currentPixelsPerMm` (sinon fallback config `microscope.anchor_pixels_per_mm`, défaut 20) et la rotation supposée (`microscope.anchor_rotation_deg`, défaut 0). Réutilise exactement la math du 2-comp align (cosR/sinR/tx/ty → 4 coins board → `Homography::compute`), mais 1 point + échelle/rotation connues au lieu de 2 points. `Application::startComponentAnchor()` + membres `m_anchorMode/m_anchorRef/m_anchorPcb` ; bloc clic prioritaire en tête du handler `CameraView::clicked` ; annule les autres modes de picking. Config : section `microscope` (anchor_pixels_per_mm, anchor_rotation_deg). UI : `m_actAnchor` dans la toolbar + signal `componentAnchorRequested`. ⚠️ Non testé — valider sur Jetson. Reste de l'Étape 1 : minimap (`BoardMinimap`, clic→ancrage) en sous-étape suivante. Étapes 2–4 (suivi incrémental, échelle live, IA) au plan.

> **2026-06-16 (suite 36)** : **profils caméra D405 ⇄ Microscope**. Pivot stratégique : la D405 couvre les composants **≥0402** (placement guidé + inspection 3D + dataset IA), le microscope (caméra originale + bague 0.3x) couvre le **0201**. Pas de dual-flux simultané : deux profils persistés, un seul actif, bascule rapide via hot-swap existant. Implémenté : (1) `struct CameraProfile` dans `Config.h` (name, backend, index, width/height/fps, hwDecode, scaleMethod, opticalMultiplier) ; `m_profiles[2]` initialisés dans le ctor — profil 0 « D405 » (RealSense, 848×480, ScaleMethod::Depth), profil 1 « Microscope » (V4L2, 1920×1080, ScaleMethod::Homography, opticalMultiplier=0.3) ; `applyActiveProfile()`/`saveCurrentCameraToProfile()` synchronisent les champs plats existants ; sérialisés en `camera_profiles[]`/`active_profile` JSON. (2) `Application::switchProfile(int)` : sauvegarde l'état tracking sortant (`m_profileStates[outIdx]` : liveMode, pixelsPerMm, matrices homographie), sauve les réglages caméra plats vers le profil sortant, applique le nouveau profil, appelle `switchCameraBackend()`, restaure l'état tracking entrant. `struct ProfileTrackingState` + `m_profileStates{2}` dans `Application.h`. Signal `cameraProfileChanged(int)`. (3) UI : `QComboBox* m_profileCombo` dans la toolbar de `MainWindow`, items « D405 » / « Microscope », signal `cameraProfileChangeRequested(int)` câblé à `Application::switchProfile` ; `setActiveProfile(int)` avec `QSignalBlocker` pour sync sans boucle. Plan `docs/MICROSCOPE_PLACEMENT_PLAN.md` créé (§0bis coexistence caméras, §1–8 localisation robuste à fort grossissement : ancrage manuel + suivi incrémental + matching contraint + estimation d'échelle live depuis l'homographie). ⚠️ Non testé — valider sur Jetson avec les deux caméras branchées.

## État actuel — au 2026-06-16 (RealSense D405 — ViewModeBar overlay + fix Controls dialog)

> **2026-06-16 (suite 35)** : **review Copilot PR #8 traitée**. Commentaire valide : la décision d'undistort dans le lambda `frameReady` (QueuedConnection) lisait le membre mutable `m_activeBackend` → race au hot-swap de backend (des frames de l'ancienne caméra encore en file pouvaient être (dé)corrigées selon la règle du nouveau backend, ex. double-correction des dernières frames RealSense après bascule V4L2). Fix : épingler le backend par connexion — `const CameraBackend backend = m_activeBackend;` capturé par valeur dans le lambda (m_activeBackend est déjà à jour pour la nouvelle caméra au moment de `wireCameraSignals()`, et la connexion meurt avec l'ancien objet caméra). Le `dynamic_cast` IR (off par défaut) laissé tel quel — bénin. 2e commentaire (doc JETSON_SESSION_LOG.md ligne PR #7 mergée) marqué outdated, ignoré. CI verte (shell + compose).

> **2026-06-16 (suite 34)** : **ViewModeBar overlay + fix RealSenseControlsDialog**. (A) **Filtre options internes RS2** : `enumerateOptions` filtre désormais `Stream Filter`, `Stream Format Filter`, `Stream Index Filter` (options internes librealsense de mux de flux, sans signification utilisateur) → plus affichés dans le panneau dynamic Controls. (B) **Boutons Outils non tronqués** : `presetRow` (Charger/Enregistrer preset JSON) passe de `QHBoxLayout` à `QVBoxLayout` (boutons l'un sous l'autre, texte complet visible) ; `setMinimumSize(440→500, 560)`. (C) **ViewModeBar** : nouveau widget overlay `gui/ViewModeBar` (QWidget transparent, child de `m_centralStack`, `raise()` permanent) affiche deux pill buttons « ● Depth » et « ● 3D » dans le coin haut-droit, visibles **quel que soit l'index de la stack** (vue 2D ou vue 3D). Les QActions `m_actDepthView` / `m_actPointCloud` restent fonctionnels (raccourcis D / 3) mais **sortis du menu View** (menu réservé aux panneaux GUI) ; `addAction(fenêtre)` pour garder les raccourcis. `setDepthViewAvailable` / `setPointCloudAvailable` appellent désormais `ViewModeBar::setDepthEnabled/setCloudEnabled`. Bouton unique CameraView (`setViewToggleVisible(false)` au démarrage) désactivé car remplacé. `MainWindow::resizeEvent` + `repositionViewModeBar` repositionne l'overlay. CMake : `ViewModeBar.{h,cpp}` ajoutés. ⚠️ Non testé — valider sur Jetson.

## État actuel — au 2026-06-16 (RealSense D405 — tuning scène statique issue #10682)

> **2026-06-16 (suite 33)** : **tuning « scène statique » issue librealsense #10682** (PDF complet du thread fourni par l'utilisateur — le screenshot était illisible, GitHub API en 403, retrouvé via WebSearch puis lecture PDF). Le thread (D405, jeoejeda ↔ MartyG-RealSense) porte sur la mesure précise au mm d'un objet **fixe** (déformation) — **exactement notre cas (carte sur banc fixe)**. 3 recommandations de MartyG appliquées : (1) **Temporal filter Smooth Alpha = 0.1** (défaut SDK 0.4) — « significantly stabilize fluctuation » sur scène statique. Appliqué comme **défaut** dans le ctor de `RealSenseCapture` (`m_filters->temporal.set_option(RS2_OPTION_FILTER_SMOOTH_ALPHA, 0.1f)`), surchargeable dans le panneau Post-Processing. (2) **Second Peak Threshold = 0** (advanced mode, défaut 325) — réduit la fluctuation depth. Nouveau `setSecondPeakThreshold(int)`/`secondPeakThreshold()` (`rs400::advanced_mode` + `STDepthControlGroup`, mêmes garde-fous que `setDisparityShift` — warn si advanced mode off ; champs `float` → cast explicite). Slider « Second Peak Threshold » (0–1023) dans Outils + intégré au profil « Inspection rapprochée (réglé) » (secondPeak=0). Tradeoff MartyG : la depth réagit plus lentement au mouvement → parfait pour carte immobile. (3) **Decimation filter** (présent dans la liste Post-Processing du Viewer montrée par MartyG, ordre canonique Intel) — `setCloudDecimation(int)`/`cloudDecimation()` : décimation appliquée **uniquement** au nuage 3D + export PLY (jamais la depth de l'overlay, dont l'alignement couleur 1:1 doit rester exact ; le SDK re-mappe la texture via UV donc une depth plus grossière colore correctement). Membre `rs2::decimation_filter` dans `FilterChain` (hors des 4 filtres toggleables → pas dupliqué dans `listControls`), combo « Décimation nuage 3D » (Off/×2/×3/×4, défaut Off) dans Outils. Autres points du thread **non** implémentés (hors scope/hardware) : Score Maximum Threshold (idem secondPeak, undocumented/ML — laissé au défaut), projecteur de motif visible (hardware), `rs2_deproject_pixel_to_point` (déjà couvert par notre nuage `rs2::pointcloud`), focal-length calibration (mire PDF Intel). ⚠️ Non testé D405 — valider sur Jetson. Advanced mode (secondPeak + disparity shift) nécessite d'être **pré-activé** sur la caméra (sinon warn, pas de toggle auto car reset device). **Build Jetson** : 2 erreurs attrapées au 1er build des nouveautés RealSense. (a) `PointCloudView.cpp:103 initializeFunctions → initializeOpenGLFunctions` (faute de frappe de suite 23, jamais compilée avant — cf [JETSON_ERREURS #20](JETSON_ERREURS.md)). (b) **Piège nom de champ SDK** : dans `STDepthControlGroup` (rs_advanced_mode), le « Second Peak Threshold » s'appelle **`deepSeaSecondPeakThreshold`** (PAS `secondPeakThreshold`) — la clé JSON correspondante reste `param-secondpeakdelta`. Corrigé dans `setSecondPeakThreshold`/`secondPeakThreshold`. Les deux corrigées et poussées.

> **2026-06-16 (suite 32)** : **caméra IR gauche D405** (Intel « Tuning depth cameras » — "Use the left-color camera"). Article screenshot analysé : recommandation software restante = utiliser la caméra IR stéréo gauche (Y8 grayscale) à la place de la couleur sur les surfaces réfléchissantes (solder, métal nu, pads) où la caméra couleur sature. Implémenté : (1) `RealSenseCapture` : `m_emitIR` atomic + `setEmitInfrared(bool)` / `emitInfrared()` + signal `infraredReady(FrameRef)`. (2) `captureLoop` : `cfg.enable_stream(RS2_STREAM_INFRARED, 1, …, RS2_FORMAT_Y8)` activé aux côtés du depth (stream toujours configuré quand `depthOn`, zéro overhead quand `m_emitIR=false`) ; bloc IR = `fs.get_infrared_frame(1)` → Y8→BGR → `emit infraredReady`. (3) `Application` : `frameReady` gate : `if (!m_depthViewMode && !irActive)` (vérifie `rs->emitInfrared()`) ; `colorizedDepthReady` gate ajuste aussi : `if (!m_depthViewMode || rs->emitInfrared() || …)` → IR a la priorité sur depth et couleur ; connexion `infraredReady` → Y8→RGB → `cameraView()->updateFrame`. (4) `RealSenseControlsDialog` : toggle ToggleSwitch « IR gauche (niveaux de gris): » dans la section Flux (sous le toggle Depth), appelle `m_camera->setEmitInfrared(on)` directement (pas de restart). ⚠️ Non testé sur D405 réelle — valider sur Jetson. À vérifier : (a) la D405 fournit bien un flux INFRARED,1 quand depth actif (devrait être le cas — même capteur stéréo) ; (b) résolution IR == résolution depth (attendu car même capteur) ; (c) l'IR est plus contrasté que la couleur sur les surfaces réfléchissantes.

> **2026-06-15 (suite 31)** : **tuning depth (guide Intel « Tuning depth cameras for best performance »)**. ⚠️ Les 2 URL de l'article étaient bloquées (403 / ECONNREFUSED) → recommandations appliquées de mémoire (article bien connu). Déjà couvert : depth units (option exposée, D405 à 0,1 mm), AE ROI, preset High Accuracy + spatial/temporal. **Ajouté** : (1) **disparity shift** via advanced mode — `RealSenseCapture::setDisparityShift(int)/disparityShift()` (`rs400::advanced_mode` + `STDepthTableControl`, `rs_advanced_mode.hpp`) : décale la fenêtre Z vers l'avant pour mesurer de très près (petits composants), clé pour la distance de travail courte D405 ; garde-fou si advanced mode désactivé (warn, pas de toggle auto car ça reset le device). Slider « Disparity shift » (0–256) dans le groupe Outils. (2) Nouveau **profil « Inspection rapprochée (réglé) »** : High Accuracy + spatial/temporal + disparity shift 64 + AE ROI centre, appliqués après restart (`applyProfile` étendu avec `disparityShift`/`aeRoiCenter` dans `UiProfile`).

> **2026-06-15 (suite 30)** : **finitions Viewer (lot final)** : (1) **section « Flux »** en haut du RealSenseControlsDialog : Color (toujours actif) + **toggle Depth** (`setDepthStreamEnabled`, redémarre la caméra pour prendre effet) + **FPS live par flux** (polling QTimer 1 Hz). `RealSenseCapture` mesure `colorFps()/depthFps()` (compteurs dans `captureLoop`, fenêtre 1 s) et `m_depthStreamEnabled` conditionne l'`enable_stream(DEPTH)` (color toujours on car flux principal). (2) **ordre des options** calqué sur le Viewer : `optionPriority()` (Visual Preset, Auto Exposure, Exposure, Gain, White Balance, Laser Power… puis le reste) + `stable_sort` par (owner, priorité) dans `rebuild()`. (3) **persistance de l'état replié** des groupes via `QSettings("PCBInspector","RealSenseControls")` clé `group/<titre>`. Combiné aux suites 28-29 : sliders, toggle switches, info ⓘ, sections Controls/Post-Processing repliables, FPS live, ordre Viewer.

> **2026-06-15 (suite 29)** : **finitions UI façon Viewer** : (1) nouveau widget `gui/ToggleSwitch` (QAbstractButton, pill animé via propriété `offset`, vert = on) remplaçant les checkboxes pour les options booléennes du RealSenseControlsDialog (capteur + « Enabled » des filtres) ; (2) **icône d'info ⓘ** à côté de chaque libellé d'option, tooltip = description SDK (curseur WhatsThis), comme le Viewer. CMake : `ToggleSwitch.{h,cpp}` ajoutés (AUTOMOC). Le reste du rendu (sliders numériques, sections Controls/Post-Processing, groupes repliables, combos enum) inchangé depuis suite 28.

> **2026-06-15 (suite 28)** : **panneau d'options aligné sur le RealSense Viewer**. Refonte du rendu de `RealSenseControlsDialog::rebuild()` : (1) options **numériques → slider + champ valeur** synchronisés (au lieu d'un simple spinbox), exactement comme le Viewer ; (2) deux **sections** distinctes avec en-têtes : « Controls » (options capteur, ownerId < kFilterBase) et « Post-Processing » (filtres, ownerId ≥ kFilterBase) ; (3) **groupes repliables** (QGroupBox checkable qui masque/affiche son contenu) ; (4) booléens → checkbox, enums (Visual Preset…) → combo avec les noms SDK, read-only → désactivé. Helpers `buildControlRow(RsControl)` et `makeCollapsibleGroup(title, form&)`. Tooltips = description SDK (inchangé). Le reste (profils, Outils, presets JSON, record) inchangé.

> **2026-06-15 (suite 27)** : **briques RealSense Viewer — lot 2 (4 de plus)**. (1) **Axes 3D** dans `PointCloudView` : gizmo XYZ (rouge/vert/bleu, 5 cm) à l'origine (= position caméra), VBO/VAO statique dessiné en `GL_LINES` avec le même shader — aide à l'orientation pendant l'orbite. (2) **Presets JSON advanced-mode** : `RealSenseCapture::saveJsonPreset/loadJsonPreset` via `rs2::serializable_device` (serialize_json/load_json), appliqués live ; boutons « Charger/Enregistrer preset JSON… » (groupe Outils) → permet de charger les presets recommandés Intel. (3) **Taux de remplissage depth** : nouvelle ligne « Depth fill: » dans StatsPanel (= % pixels depth non nuls sur la frame, `cv::countNonZero`, calculé à 3 Hz dans le handler depth) ; vert ≥80 %, orange 50–80 %, rouge <50 % — indicateur de trous (surfaces lisses/brillantes, trop près/loin). `StatsPanel::setFillRate`. (4) **Enregistrement `.bag`** : `setRecordFile(path)` → `cfg.enable_record_to_file()` appliqué au prochain (re)start (comme le bouton Record du Viewer) ; bouton checkable « Enregistrer en .bag… » (file dialog, prend effet au redémarrage caméra). ⚠️ Lot 1 (suite 26) + lot 2 non testés ici — valider sur D405 réelle. La **relecture** `.bag` (source `enable_device_from_file`) n'est PAS faite (changerait toute la source de capture) — à planifier si besoin.

> **2026-06-15 (suite 26)** : **briques RealSense Viewer ajoutées (4)**. (1) **Colorizer** : la vue depth 2D utilise désormais `rs2::colorizer` (scheme Jet + égalisation d'histogramme, comme le panneau Depth du Viewer) calculé dans le thread capture → signal `colorizedDepthReady` (BGR Mat) ; remplace le `COLORMAP_JET` manuel à plage fixe. Gated par `setEmitColorizedDepth` (piloté par le toggle depth-view). (2) **Auto-exposition ROI** : `setAutoExposureRoi(x,y,w,h)` via `rs2::roi_sensor::set_region_of_interest` (défaut = centre 50%), appliqué live sur le device. Bouton « Auto-exposition sur le centre » dans le RealSenseControlsDialog (groupe Outils). (3) **Export PLY** : `requestPlyExport(path)` → le thread capture appelle `pts.export_to_ply(path, color)` sur le prochain nuage (indépendant du mode 3D) → signal `plyExportFinished`. Bouton « Exporter le nuage 3D (PLY)… » (file dialog). (4) **Self-calibration on-chip** : `requestOnChipCalibration()` → le thread capture lance `auto_calibrated_device::run_on_chip_calibration` (speed=slow, ~10 s, bloque la boucle), `set_calibration_table` + `write_calibration`, renvoie le health → signal `onChipCalibrationFinished`. Bouton « Self-calibration depth (sans mire)… » avec confirmation. ⚠️ Self-cal **expérimental** (non testable ici) : valider sur D405 réelle, vérifier que `run_on_chip_calibration` accepte la config et que le health est cohérent (proche de 0 = bon).

> **2026-06-15 (suite 25)** : **ordre de post-processing depth aligné sur le whitepaper Intel / exemple `rs-post-processing`**. WebFetch confirme l'ordre canonique : Threshold → **depth→disparity** → Spatial → Temporal → **disparity→depth** → Hole Filling (spatial+temporal s'appliquent dans le **domaine disparité**, bracketé par `rs2::disparity_transform(true/false)`). Avant : on appliquait spatial/temporal/threshold/holeFill directement sur la depth, dans l'ordre d'index (spatial avant threshold, sans transform disparité). Maintenant : `FilterChain::processOrdered()` respecte l'ordre Intel, avec les deux `disparity_transform` membres (non exposés à l'UI, activés auto si spatial **ou** temporal est on). Les toggles utilisateur (spatial/temporal/threshold/holeFill) restent inchangés. **Nuage 3D** : recalé pour réutiliser la **même depth filtrée** (alignée) que `depthFrameReady` au lieu de la depth brute — bénéficie du lissage spatial/temporal sans faire tourner le temporal filter sur un 2e flux (corruption d'état évitée). Presets visuels (High Accuracy / Medium / High Density) déjà pilotés par le SDK via `get_option_value_description` (matching robuste, skip si absent sur la D405) — pas de changement nécessaire.

> **2026-06-15 (suite 24)** : **nuage 3D via la voie SDK `rs2::pointcloud`** (alignement sur le RealSense Viewer / exemple `rs-pointcloud`, suggéré par l'utilisateur). Avant : déprojection manuelle de la depth alignée dans le thread GUI (suite 23). Maintenant : calcul **dans le thread capture** via `pc.map_to(color)` + `pc.calculate(rawDepth)` (depth NON alignée — le SDK applique intrinsèques depth + extrinsèques depth→couleur, et fournit des **texcoords UV** par point). Méthode canonique confirmée par WebFetch des sources Intel : vertices en **mètres** repère caméra (X→droite, Y→bas, Z→avant), `vertices[i].z==0` ignoré. Nouveau type partagé `camera/PointCloudData.h` (`PointCloudRef = shared_ptr<const {xyz, rgb, count}>`, metatype enregistré) — la traversée de thread ne copie que le shared_ptr. `RealSenseCapture` : `setEmitPointCloud(bool)` (off par défaut, activé seulement quand la vue 3D est affichée), signal `pointCloudReady`, émission throttlée ~15 Hz, downsample < 200k points, couleur échantillonnée en BGR8 via les texcoords. `PointCloudView::updateCloud(depth,…)` remplacé par `setCloud(PointCloudRef)` (juste l'interleave x,-y,-z,r,g,b + upload VBO ; unités mètres → perspective near/far 0.01–100 m, zoom 0.02–50 m). `Application` connecte `pointCloudReady → setCloud` et pilote `setEmitPointCloud` au toggle + à la (re)connexion caméra. Avantages : thread GUI déchargé, géométrie/texture gérées par le SDK testé, pas de dépendance à notre alignement pour la 3D.

> **2026-06-15 (suite 23)** : **nuage de points 3D interactif** (panneau « 3D » façon RealSense Viewer). Nouveau widget `gui/PointCloudView` (`QOpenGLWidget` + `QOpenGLFunctions`, VBO + VAO, shader portable GL 3.3 core / GLES 3.0 choisi à l'exécution via `context()->isOpenGLES()`). `updateCloud(depthMm, colorBgr, fx, fy, ppx, ppy)` déprojette chaque pixel depth valide via les intrinsèques couleur (`X=(u-ppx)·Z/fx`, `Y=(v-ppy)·Z/fy`, Y/Z inversés pour une vue naturelle), échantillonne la couleur alignée, downsample auto pour rester < 200k points. Caméra orbit : clic gauche = rotation (yaw/pitch), molette = zoom (distance), clic milieu/droit = pan ; cible recadrée sur le centroïde au 1er nuage. Central de `MainWindow` passé en `QStackedWidget` {CameraView idx0, PointCloudView idx1} — fullscreen inchangé (agit sur la fenêtre). Menu **View → « 3D Point Cloud »** (raccourci **3**, activé seulement si backend RealSense via `setPointCloudAvailable`). `Application` : `m_pointCloudMode` piloté par `pointCloudToggled` ; dans le handler depth, build du nuage throttlé ~12 Hz avec `rs->latestFrame()` comme couleur. CMake : `PointCloudView.{h,cpp}` ajoutés (AUTOMOC). Fallback gracieux : si le shader ne compile pas, message QPainter au lieu d'un crash. ⚠️ À valider sur Jetson — selon le contexte GL fourni (EGL/GLES vs desktop GL via driver NVIDIA), ajuster la version de shader si build/rendu KO.

> **2026-06-15 (suite 22)** : **bouton de bascule de vue en surimpression** (demande utilisateur). En plus du menu View → Depth View, un **bouton « pill » semi-transparent** est dessiné en haut à droite de l'image (`CameraView::drawViewToggle`, dans `paintEvent` comme le crosshair/zoom — pas de widget posé sur le QOpenGLWidget). Libellé = mode cible (« ● Depth » en mode couleur, « ● Color » en mode depth, fond bleuté quand depth actif). Clic hit-testé dans `mousePressEvent` via `m_viewToggleRect` (prioritaire sur le clic image). Signal `CameraView::viewToggleClicked` → `MainWindow` fait `m_actDepthView->toggle()` (menu + bouton synchronisés dans les deux sens via `setDepthViewActive`). Visible seulement quand le backend RealSense streame (`setDepthViewAvailable` → `setViewToggleVisible`). Aussi : fix UI texte tronqué dans Controls (suite, commit séparé) — dock 320→400px, libellés boutons raccourcis, combo Device autorisé à rétrécir.

> **2026-06-15 (suite 21)** : **vue Depth colorisée + fix log power state**. (1) Nouveau **mode d'affichage Depth** (View → « Depth View (colorized) », raccourci **D**) : colorise la depth alignée (déjà émise par `RealSenseCapture::depthFrameReady`) en COLORMAP_JET sur la plage de travail D405 ~30–600 mm (0 = invalide → noir) et l'affiche dans le `CameraView` à la cadence du flux depth (pas de throttle, contrairement au calcul distance/scale qui reste à 3 Hz). `Application::m_depthViewMode` gate le `updateFrame(display)` couleur dans `frameReady` (l'overlay iBOM continue d'être composité par-dessus la depth — coords identiques car depth alignée couleur). Signal `MainWindow::depthViewToggled(bool)`, action `m_actDepthView` activée seulement en backend RealSense (`setDepthViewAvailable` appelé depuis `updateCalibrationUI`). **Note** : c'est la vue 2D depth (équivalent panneau « Depth » du Viewer Intel) ; le **vrai nuage de points 3D interactif** (panneau « 3D » du Viewer, rendu OpenGL orbitable) reste un chantier séparé plus lourd — à planifier. (2) `RealSenseCapture::listDevices` : warning `failed to set power state` (énumération pendant que la caméra streame, ex. réouverture de Settings) passé en `spdlog::debug` — bénin/transitoire, la liste existante reste valide.

> **2026-06-15 (suite 20)** : **masquage W/H/FPS dans le ControlPanel pour RealSense** (suite suite 19). Le groupe Camera du ControlPanel (dock droit) affichait encore Width/Height/FPS avec 1920×1080 quand RealSense actif. Fix : `createCameraGroup()` wraps les spinboxes W/H/FPS + bouton Apply dans `m_camResWidget` (QWidget container) ; `setCameraBackendUI(bool)` appelle `m_camResWidget->setVisible(!isRealSense)`. La ligne Device reste visible pour les deux backends. Migration Config : si `backend=realsense` et `width=1920, height=1080` encore stocké → reset à 848×480@30 automatiquement (plus de warning "unsupported" au premier lancement). Fix label 480×270@90 → @60fps (D405 supporte 60fps à cette résolution, pas 90).

> **2026-06-15 (suite 19)** : **sélecteur de profil résolution RealSense dans Settings**. Quand le backend RealSense est sélectionné dans Settings → Camera, les spinboxes W/H/FPS (sans signification pour la D405) sont masqués et remplacés par un combo « Resolution: » avec 3 profils D405 : `848×480@30fps (Depth Precision, recommended)`, `1280×720@30fps (Visual Detail)`, `480×270@90fps (Fast Preview)`. Masquage via `m_v4l2ResWidget.setVisible(!isRS)` / `m_rsResWidget.setVisible(isRS)`. Le profil est sélectionné automatiquement au chargement en matchant `cameraWidth` stocké (≥1280→HD, ≤480→preview, sinon 848×480). En `accept()`, les valeurs W/H/FPS réelles (`kRsProfiles[]`) sont écrites en config selon le profil choisi (pas les spinboxes). Le changement de backend (`currentIndexChanged`) appelle `updateCameraResolutionUI()`. Plus de 1920×1080 visible pour RealSense.

> **2026-06-15 (suite 18)** : **fix build Jetson `SettingsDialog::accept() is private`** (erreur #19). L'override `accept()` (ajouté en suite 12 pour fermer Settings avant le panneau RealSense) était sous `private slots:` → `MainWindow::onShowSettings` (`dlg.accept()`) ne compilait pas. Déplacé sous `public slots:` (cohérent avec `QDialog`). ⚠️ Bug hérité par `main` via PR #7, corrigé par PR #8. Rappel : la CI ne compile pas le C++, seul un build Jetson attrape ça.
>
> **2026-06-15 (suite 17)** : **regroupement calibration/alignement (backend-aware)**. Avant : éparpillé sur 4 endroits (menu Camera : Calibrate + Generate Checkerboard + Open PDF ; barre d'outils : Calibrate ; ControlPanel groupe Camera : bouton RealSense ; ControlPanel groupe Actions : Calibrate + 2 alignements + live tracking). Maintenant : **un seul groupe « Calibration & Alignment » dans le ControlPanel (droite)**, adaptatif selon la caméra. `ControlPanel::createCalibrationGroup()` remplace `createActionsGroup()` ; `setCameraBackendUI(bool)` **masque/affiche** (ne grise plus) : microscope → bouton Calibrate + Generate/Print Checkerboard + Open PDF + label d'aide ; RealSense → bouton Camera Controls + label « factory-calibrated, voir Statistics ». Alignement (4 coins / 2 composants) + Live Tracking visibles pour les deux (clic sur l'image → restent hors dialogue). Nouveaux signaux `generateCheckerboardRequested()` / `openCalibrationPdfRequested()` câblés aux slots MainWindow existants. **Menu Camera** réduit à Toggle Camera ; **barre d'outils** sans bouton Calibrate. Raccourci **K** conservé (action ajoutée à la fenêtre via `addAction`). Les guides Help (Calibration/Alignment) restent (documentation, pas des contrôles).
>
> **2026-06-15 (suite 16)** : **tooltip intrinsèques RealSense enrichi**. `RealSenseCapture` cache désormais aussi `fy`, `ppx`, `ppy` (en plus de `fx`) des intrinsèques usine couleur → accesseurs `colorFy()/colorPpx()/colorPpy()`. `StatsPanel::setCalibration` accepte un `tooltip` optionnel. `Application::updateCalibrationUI` calcule le **FOV** depuis le modèle pinhole (`FOV = 2·atan(taille / 2·f)`) et affiche au survol de la ligne « Calibration: » : résolution, fx/fy, point principal, FOV H×V, + rappel `rs-enumerate-devices -c`. Pour la D405 à 848×480 : fx≈437 px → ~88° H, cohérent avec la spec (~87°). Le `fx` n'est pas une note qualité mais une constante physique ; vérif = proportionnalité à la largeur (436.8@848 ↔ 655.2@1280, ratio 1.50) + correspondance FOV.
>
> **2026-06-15 (suite 15)** : **calibration dans le GUI selon le backend**. `StatsPanel` : nouvelle ligne « Calibration: » dans Performance → V4L2 calibré = `RMS 0.13  1.41 px/mm` (vert si RMS<1, orange sinon) ; V4L2 non calibré = `—` ; RealSense = `Factory  fx=436.8 px` (vert, intrinsèques usine SDK). `ControlPanel::setCameraBackendUI(bool)` → désactive le bouton « Calibrate Camera » et change son texte en « Calibration (Factory — RealSense SDK) » quand D405 actif (le SDK a les intrinsèques, le checkerboard ne s'applique pas). `CameraCalibration` : nouveau `rmsError()` + persisté dans le YAML (`rms_error`). `Application::updateCalibrationUI()` → appelé par `wireCameraSignals()` (changement de backend) + `runCalibration()` (fin de calibration réussie). **Action sur Jetson** : supprimer `calibration.yml` (l'ancien n'a pas le champ `rms_error`), rebuildez.
>
> **2026-06-15 (suite 14)** : **nettoyage des logs au démarrage D405** (4 fixes, branche `claude/fix-realsense-undistort`, PR #8). (1) `QFont::setPixelSize: Pixel size <= 0 (0)` ×7 → causé par `font-size: 0px` (hack pour cacher le texte %) sur la `QProgressBar` des deux thèmes → remplacé par `color: transparent` (MainWindow.cpp). (2) `RealSense 1920x1080 unsupported, retrying` → le handler `settingsChanged` poussait la résolution générique 1920×1080 du Config sur la D405 à chaque OK de Settings (même trou que `createCamera`) → même guard `skipRes` ajouté (plus de fallback + stop/start parasite). (3) Scan V4L2 `can't open camera by index` video0-9 alors que le backend est RealSense → `SettingsDialog` appelait `enumerateCameras()` dans le constructeur **avant** `loadFromConfig` (combo backend encore à V4L2) → déplacé dans `loadFromConfig` après sync du combo (`QSignalBlocker`), retiré du constructeur. (4) `[error] Cannot open calibration file` au 1er lancement → `CameraCalibration::load` teste `std::filesystem::exists` d'abord → log `debug` si absent (supprime aussi le `[ERROR]` interne OpenCV FileStorage). Restes bénins non touchés : `Segoe UI` (police Windows, substituée par Qt sur Linux), `XDG_RUNTIME_DIR not set` (Docker).
>
> **2026-06-15 (suite 13)** : **fix calibration OpenCV désactivée pour RealSense** — librealsense applique déjà les intrinsèques usine, appliquer `undistort` OpenCV par-dessus déformait l'image. Condition dans `Application::wireCameraSignals` : `undistort` ignoré si `m_activeBackend == CameraBackend::RealSense`. Supprimer `/opt/microscope-ibom/data/calibration.yml` sur Jetson pour effacer la calibration corrompue. PR #7 mergée dans `main` (commit `e2b8e66`). ✅ D405 testé sur Jetson : couleur OK, depth/scale OK, RMS calibration 0.13 (mais ne pas réappliquer pour RealSense). Problème "écran non rempli" = letterbox normal (16:9 CameraView + panneaux dock latéraux = ratio widget ≠ 16:9 image) — voir note ci-dessous.
>
> **2026-06-15 (suite 12)** : **contrôles RealSense aussi accessibles depuis Settings**. Bouton « Camera Controls (RealSense)… » ajouté dans l'onglet Camera du `SettingsDialog` → signal `realSenseControlsRequested()`. `MainWindow::onShowSettings` ferme Settings (évite le blocage modal) puis émet `MainWindow::realSenseControlsRequested()`. Handler `Application::openRealSenseControls()` extrait en méthode, connecté **à la fois** au bouton du ControlPanel et au signal de MainWindow.
>
> **2026-06-15 (suite 11)** : **review Copilot PR #7 traitée** (6 points). (1+5) `RealSenseCapture::stop()` join/reset toujours le thread même si la boucle s'est arrêtée seule ; `captureLoop` clear `m_capturing` + émet `captureStateChanged(false)` en sortie d'erreur (sinon `isCapturing()` reste vrai et bloque le restart). (2+3) Scripts : détection D405 sur PID exact `8086:0b5b` (au lieu de `8086:0b` trop large). (4) En-tête de REALSENSE_D405_PLAN.md passé en « implémenté ». (6) `RealSenseControlsDialog` : `QPointer` sur la caméra + fermeture auto sur `destroyed` + guards `if(m_camera)` dans les callbacks → plus de pointeur pendant si hot-swap pendant que le dialog est ouvert.
>
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
