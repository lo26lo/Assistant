# Plan d'amélioration du Live Tracking

> ⚠️ **Ce plan a été implémenté** (Phases 1–3, PR #20, buildé et validé sur Jetson — suites 86-98). Pour l'état des lieux **post-implémentation** et la nouvelle vague d'améliorations, voir [LIVE_TRACKING_ANALYSE_2026-07.md](LIVE_TRACKING_ANALYSE_2026-07.md).

> **But** : réduire le tremblement ("jitter") de l'overlay en live tracking, améliorer la fluidité et la robustesse du suivi, et exploiter le GPU du Jetson AGX Orin.
>
> **Contexte** : suivi ORB + BFMatcher (Lowe) + `findHomography(RANSAC)` dans `src/overlay/TrackingWorker.{h,cpp}`, sur thread dédié. Lissage par blend des coins ajouté en suite 82 (ERREUR #47). Retour utilisateur : « c'est pas mal mais ça vibre si je ne bouge rien ».
>
> **Contrainte de validation** : pas de toolchain Qt6/OpenCV dans l'environnement de dev (cloud) → **toute modif CV doit être testée au build Jetson**, idéalement une phase à la fois.
>
> **Documents liés** : [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) · [JETSON_ERREURS.md](JETSON_ERREURS.md) (#47 jitter) · [MICROSCOPE_PLACEMENT_PLAN.md](MICROSCOPE_PLACEMENT_PLAN.md)

---

## 1. Diagnostic — d'où vient l'instabilité

Trois sources de bruit frame-à-frame, **même sur scène strictement immobile** :

1. **RANSAC randomisé** — `cv::findHomography(..., cv::RANSAC)` tire un sous-ensemble d'inliers différent à chaque appel → l'homographie estimée varie de quelques dixièmes de pixel d'une frame à l'autre.
2. **Keypoints ORB quantifiés** — positions arrondies à l'entier, **sur l'image downscalée ×0.5** → au remap ×2 vers la pleine résolution, le bruit sub-pixel est doublé.
3. **Sur-paramétrisation** — une homographie a 8 DDL ; une carte ~plane et fronto-parallèle ne bouge qu'en similitude (≈4 DDL : tx, ty, rotation, échelle). Les 4 DDL excédentaires "absorbent" le bruit sous forme de micro-perspective → tremblement visible surtout aux bords de l'overlay.

Le lissage actuel (`smoothHomography`, suite 82) **masque** le symptôme par un EMA adaptatif sur les coins. Les propositions ci-dessous **attaquent les causes** et ajoutent un filtrage de meilleure qualité.

---

## 2. Idées issues de l'état de l'art (recherche web, juin 2026)

| Source | Idée applicable ici |
|--------|---------------------|
| **1€ Filter** (Casiez, Roussel, Vogel — CHI 2012) | Filtre passe-bas **à fréquence de coupure adaptative à la vitesse** : à l'arrêt, coupure basse → jitter écrasé ; en mouvement, coupure haute → pas de lag. 2 paramètres, ~20 lignes. **Meilleur compromis jitter/lag** documenté pour entrées interactives. ⇒ remplace avantageusement notre EMA à seuils. |
| **Video stabilization / motion smoothing** (IPOL 2017, Lee & Chuang 2009) | Lisser une **trajectoire de transformations** plutôt qu'une seule frame ; choisir le **modèle de mouvement** (translation / similitude / affine / homographie) selon la scène — l'homographie complète n'est utile que si la perspective est réelle. |
| **Kalman sur paramètres de mouvement** (SURF + Kalman, classique) | Filtrer dans un espace **décomposé** (tx, ty, θ, scale) avec un modèle vitesse-constante → prédiction = anti-lag, covariance = anti-jitter. |
| **Lucas-Kanade optical flow** (USPTO "reference-free drift-corrected planar tracking", NVIDIA OpenCV optical flow blog) | LK donne une **stabilité ~0.9 px contre 1.5–2.8 px** pour les méthodes par re-détection. Suivre les inliers en sub-pixel entre frames est intrinsèquement plus lisse que re-détecter ORB à chaque frame. |
| **GPU ORB / optical flow sur Jetson** (NVIDIA, OpenCV CUDA) | `cv::cuda::ORB` ≈ ×11 plus rapide que l'ORB CPU ; `cv::cuda::SparsePyrLKOpticalFlow` temps réel sur Orin. Le budget temps libéré permet **plus de keypoints / pleine résolution** → estimation mieux conditionnée **et** plus stable. |

Notre OpenCV Jetson est compilé `WITH_CUDA=ON` + `opencv_contrib` (arch 8.7) → modules `cudafeatures2d` et `cudaoptflow` **disponibles** (voir `docker/base.Dockerfile`).

---

## 3. Plan par phases

Chaque item liste : *mécanisme* · *fichiers* · *risque* · *effet attendu*.

### Phase 1 — gains rapides, faible risque (cible : tuer le jitter à l'arrêt)

**1.1 Détection de scène statique → ne pas ré-estimer**
*Mécanisme* : mesurer le déplacement max des coins board projetés (déjà calculé dans `smoothHomography`). S'il reste < ~0,8 px sur N frames consécutives, **ne pas ré-émettre** d'homographie (garder la pose courante). Reprendre l'estimation dès qu'un mouvement est détecté.
*Fichiers* : `TrackingWorker.cpp`.
*Risque* : faible. *Effet* : supprime ~totalement le tremblement quand rien ne bouge — **réponse directe au retour utilisateur**.

**1.2 Estimateur déterministe USAC_MAGSAC**
*Mécanisme* : `findHomography(..., cv::USAC_MAGSAC, thr)` au lieu de `cv::RANSAC` ; fixer le seed (`cv::setRNGSeed`) au démarrage du worker.
*Fichiers* : `TrackingWorker.cpp`.
*Risque* : faible (API standard OpenCV 4.10). *Effet* : élimine la variation due au tirage aléatoire ; MAGSAC est aussi plus précis sur le seuil.

**1.3 Raffinement sub-pixel des points matchés**
*Mécanisme* : avant `findHomography`, raffiner les coords des points (au moins les inliers) avec `cv::cornerSubPix` sur l'image grise pleine résolution.
*Fichiers* : `TrackingWorker.cpp`.
*Risque* : faible (coût CPU modéré ; bornable au nombre d'inliers). *Effet* : réduit le jitter **à la source** (cause #2).

**1.4 Gating qualité + hystérésis avant émission**
*Mécanisme* : n'émettre que si `inliers ≥ seuilHaut` ET `reprojErr ≤ seuilBas`. Sinon **figer** la dernière bonne pose (ne rien émettre) plutôt qu'émettre une estimée dégradée. Hystérésis pour éviter le clignotement Locked/Drifting.
*Fichiers* : `TrackingWorker.cpp` (+ états déjà présents `State::Locked/Drifting/Lost`).
*Risque* : faible. *Effet* : plus de "sauts" quand le matching se dégrade (main qui passe, glare, occlusion partielle).

### Phase 2 — qualité du modèle & du filtrage (impact fort, effort moyen)

**2.1 1€ Filter sur la pose décomposée**
*Mécanisme* : décomposer chaque homographie émise en (tx, ty, θ, scale) [+ éventuellement les 2 termes de perspective], filtrer chaque canal avec un **1€ Filter** (coupure adaptative à la vitesse), puis recomposer. Remplace `smoothHomography` (EMA coins).
*Fichiers* : nouveau `src/overlay/OneEuroFilter.h` (header-only, ~30 lignes) + `TrackingWorker.cpp`.
*Risque* : moyen (recomposition à valider). *Effet* : meilleur compromis jitter/lag que l'EMA actuel, paramétrage intuitif (2 réglages).

**2.2 Modèle de mouvement adaptatif**
*Mécanisme* : estimer d'abord une **similitude** (`estimateAffinePartial2D`) ou affine (`estimateAffine2D`) ; ne basculer sur l'homographie complète que si la perspective est significative (test sur l'erreur résiduelle / les termes h31,h32). Exposer un mode dans `Config` (`trackingModel`: similarity/affine/homography/auto).
*Fichiers* : `TrackingWorker.cpp`, `Config.{h,cpp}`, `SettingsDialog` (onglet Tracking).
*Risque* : moyen. *Effet* : overlay nettement plus stable dans le cas usuel carte plane (cause #3).

**2.3 Distribution spatiale des keypoints**
*Mécanisme* : répartir les ORB sur toute la carte (bucketing par grille ou ANMS) au lieu de grappes denses ; monter à ~400–500 keypoints (marge GPU/CPU Orin).
*Fichiers* : `TrackingWorker.cpp`, `Config` (`orbKeypoints`).
*Risque* : moyen. *Effet* : homographie mieux conditionnée → moins de wobble aux bords.

### Phase 3 — suivi sub-pixel & robustesse lumière + GPU (effort élevé)

**3.1 Optical flow (Lucas-Kanade) pour les petits mouvements**
*Mécanisme* : entre deux frames, suivre les inliers avec `calcOpticalFlowPyrLK` (sub-pixel) et re-fitter le modèle ; ne re-détecter ORB qu'à la perte de suivi ou périodiquement (ré-ancrage). Hybride flow (fluide) + ORB (ré-acquisition).
*Fichiers* : `TrackingWorker.cpp`.
*Risque* : élevé (gestion perte/ré-acquisition). *Effet* : suivi très fluide et stable (~0,9 px) sur petits déplacements.

**3.2 Pré-traitement photométrique (CLAHE)**
*Mécanisme* : `cv::createCLAHE()` sur l'image grise avant ORB.
*Fichiers* : `TrackingWorker.cpp`.
*Risque* : faible-moyen (coût). *Effet* : keypoints plus stables sous glare/ombres D405 (point noir connu).

**3.3 Accélération GPU Jetson**
*Mécanisme* : porter le chemin chaud sur CUDA — `cv::cuda::ORB`, `cv::cuda::DescriptorMatcher` (Hamming), `cv::cuda::SparsePyrLKOpticalFlow`. Upload/ download minimal (garder les frames en `GpuMat`). Gros budget temps libéré → **pleine résolution + plus de keypoints** = meilleure précision **et** stabilité, sans baisser le FPS.
*Fichiers* : `TrackingWorker.cpp` (chemin GPU conditionnel `#ifdef HAVE_OPENCV_CUDAFEATURES2D` / détection runtime `cv::cuda::getCudaEnabledDeviceCount()`), `CMakeLists.txt` (link `opencv_cudafeatures2d opencv_cudaoptflow`).
*Risque* : élevé (build, gestion mémoire GPU, fallback CPU). *Effet* : permet d'augmenter qualité sans coût FPS ; prérequis des autres gains haute-résolution.
*Prérequis* : OK — OpenCV Jetson déjà compilé `WITH_CUDA` + contrib (arch 8.7).

---

## 4. Recommandation d'ordre

1. **Phase 1** d'abord (1.1 → 1.4) : meilleur ROI, faible risque, règle le retour "ça vibre à l'arrêt". Valider sur Jetson.
2. **2.1 (1€ Filter)** puis **2.2 (modèle adaptatif)** : les deux plus gros gains de stabilité perçue.
3. **3.3 (GPU)** quand on voudra monter en résolution/keypoints ; **3.1 (optical flow)** pour la fluidité fine.
4. **2.3 / 3.2** en polish.

## 5. Réglages à exposer (Config / SettingsDialog → onglet Tracking)

- `trackingModel` : auto / similarity / affine / homography (2.2)
- `staticSceneThreshPx` + `staticSceneFrames` (1.1)
- `oneEuroMinCutoff`, `oneEuroBeta` (2.1)
- `trackingUseGpu` : auto / on / off (3.3)
- `trackingUseOpticalFlow` : on/off (3.1)
- (existants : `orbKeypoints`, `minMatchCount`, `matchDistanceRatio`, `ransacThreshold`, `trackingDownscale`, `trackingIntervalMs`)

## 6. Comment valider (sur Jetson)

1. **Test "immobile"** : caméra + carte fixes 30 s → mesurer le déplacement max des coins de l'overlay (doit tomber à ~0 avec 1.1).
2. **Test "mouvement lent"** : translation/rotation douce → vérifier l'absence de lag perceptible (1€ Filter / static-detect ne doivent pas "coller").
3. **Test "perturbation"** : main qui passe, glare → vérifier que l'overlay **fige** au lieu de sauter (1.4).
4. Comparer FPS et `reprojErrPx` (logs spdlog::debug déjà présents) avant/après, CPU vs GPU.

---

## Sources

- [1€ Filter — Casiez et al., CHI 2012 (PDF)](https://direction.bordeaux.inria.fr/~roussel/publications/2012-CHI-one-euro-filter.pdf) · [implémentations](https://github.com/casiez/OneEuroFilter)
- [Motion Smoothing Strategies for 2D Video Stabilization — IPOL 2017 (PDF)](https://www.ipol.im/pub/art/2017/209/revisions/2022-01-01/article.pdf)
- [Video Stabilization using Robust Feature Trajectories — Lee & Chuang 2009 (PDF)](https://www.csie.ntu.edu.tw/~cyy/publications/papers/Lee2009VSR.pdf)
- [Framework for reference-free drift-corrected planar tracking using Lucas-Kanade optical flow (USPTO)](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9014421)
- [cv::cuda::SparsePyrLKOpticalFlow — OpenCV docs](https://docs.opencv.org/4.x/d7/d05/classcv_1_1cuda_1_1SparsePyrLKOpticalFlow.html)
- [Accelerate OpenCV: Optical Flow Algorithms with NVIDIA GPUs — NVIDIA blog](https://developer.nvidia.com/blog/opencv-optical-flow-algorithms-with-nvidia-turing-gpus/)
- [Accelerating ORB feature extraction with CUDA — neumanncondition](https://neumanncondition.com/articles/accelerating-feature-extraction-and-image-stitching-algorithm/)
- [Building OpenCV with CUDA on Jetson Orin — Cytron tutorial](https://www.cytron.io/tutorial/build-opencv-with-cuda-support-for-jetson)
