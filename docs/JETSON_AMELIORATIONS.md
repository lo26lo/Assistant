# Conseils d'amélioration — MicroscopeIBOM sur Jetson

> **Date de l'analyse** : 2026-06-10
> **Contexte** : l'application se lance sur le Jetson AGX Orin (GUI Qt sur écran local via `scripts/run_local_gui.sh`). L'utilisateur a dû corriger à la main `docker/compose.local.yml` à cause de doublons `video`/`plugdev`.
> **Périmètre analysé** : stack Docker (`docker/`), scripts (`scripts/`), code C++ (`src/`), CMake, tests, process de documentation.
>
> Chaque point indique : le constat, pourquoi c'est un problème, et la correction conseillée. Le tableau de priorisation est en fin de document.

---

## 0. Statut d'implémentation — session 2026-06-10

La majorité des recommandations ont été **implémentées** dans la foulée de l'audit (commit de cette session). Détail dans `JETSON_SESSION_LOG.md`.

| Réf | Recommandation | Statut |
|-----|----------------|--------|
| 1.1 | Doublons `group_add` | ✅ Fait (erreur #14) |
| 1.2 | `.dockerignore` vs `runtime.Dockerfile` | ✅ Fait |
| 1.4 | restart `on-failure:3` (runtime) | ✅ Fait |
| 1.6 | Retrait `QT_QPA_GENERIC_PLUGINS`/evdevtouch | ✅ Fait |
| 1.7 | entrypoint : check GPU Tegra + bloc engines | ✅ Fait |
| 2.1 | Retrait `--force-recreate` | ✅ Fait |
| 2.2 | Cookie xauth réel | ✅ Fait |
| 2.3 | run_local_gui debug → `build-debug/bin` | ✅ Fait |
| 3.1 | FOURCC MJPG + log du format réel | ✅ Fait |
| 3.2 | Persistance unifiée `IBOM_DATA_DIR` | ✅ Fait (`src/utils/Paths.*`, Config/Application/InferenceEngine, compose, entrypoint) |
| 3.3 | Arch CPU configurable (`IBOM_TARGET_CPU`) | ✅ Fait |
| 3.5 | `test_tracking_worker.cpp` | ✅ Fait |
| 4.2 | CI GitHub Actions | 🟡 Partiel — shell + compose verts ; pour le C++, voir note ci-dessous |
| 1.3 | Image runtime réellement minimale | ✅ Fait (multi-stage l4t-jetpack + libs runtime, **jamais buildée — à valider**) |
| 3.6 | Nettoyage résidus Windows (Phase 1c) | ✅ Fait — plus aucun `#ifdef` Windows dans src/, `.bat` supprimés, CMake Linux-only |
| — | **Activation pipeline IA** (hors audit, demandé ensuite) | ✅ Fait — init en thread d'arrière-plan si `.onnx` présent ; cf [AI_PIPELINE.md](AI_PIPELINE.md) |
| 1.5 | User non-root + GID numériques | ⏸ Reporté (à faire avec le mode kiosque) |
| 1.8 | Tags d'images datés | ⏸ Process (à appliquer au prochain build) |

> ⚠️ **Important — à valider sur le Jetson** : ces changements (notamment le helper `IBOM_DATA_DIR` et le test Qt/MOC) **n'ont pas pu être compilés** dans l'environnement d'analyse (pas de CUDA/Qt/OpenCV). Faire un `bash scripts/build_jetson.sh` + `ctest` sur le Jetson pour confirmer avant de s'appuyer dessus. La 1ère exécution déplacera la config/calibration vers `~/.local/share/MicroscopeIBOM` (ou `$IBOM_DATA_DIR`) — récupérer manuellement un éventuel `calibration.yml` existant si besoin.

> **CI build C++ (4.2, reste à faire)** : décision utilisateur 2026-06-10 — **ONNX ne sera jamais optionnel** (c'est le cœur du projet Jetson), donc pas de build no-GPU appauvri. La voie retenue pour une vraie CI C++ : **runner GitHub Actions self-hosted sur le Jetson** qui builde dans le container dev et lance `ctest` avec le stack complet (CUDA/ONNX/TensorRT). À mettre en place quand souhaité.

---

## 1. Docker & Compose

### 1.1 ✅ CORRIGÉ — Doublons `group_add` entre `compose.yml` et `compose.local.yml`

**Constat** : Docker Compose **concatène** les listes (`group_add`, `devices`, `volumes`…) lors d'un merge d'override, il ne les remplace pas. `compose.yml` (service `dev`) déclare déjà `group_add: [video, plugdev, dialout]` ; `compose.local.yml` redéclarait `[video, input, plugdev]` → `video` et `plugdev` en double dans la config finale.

**Fix appliqué** (commit de cette session) : l'override ne déclare plus que ce qui est *ajouté*, c'est-à-dire `input`. Entrée détaillée : `JETSON_ERREURS.md` erreur #14.

> ⚠️ **Sur le Jetson** : avant le prochain `git pull`, restaurer le fichier modifié localement (`git checkout docker/compose.local.yml`) pour éviter un conflit, puis vérifier que la version du repo correspond au fix local.

### 1.2 🔴 BLOQUANT (futur) — `runtime.Dockerfile` ne peut pas builder : fichiers exclus par `.dockerignore`

**Constat** : `.dockerignore` exclut `build/` **et** `docker/`, mais `docker/runtime.Dockerfile` fait :
```dockerfile
COPY build/bin/MicroscopeIBOM /opt/microscope-ibom/MicroscopeIBOM   # build/ ignoré !
COPY docker/entrypoint.sh /entrypoint.sh                            # docker/ ignoré !
```
Les deux `COPY` échoueront avec "not found" au premier `docker compose build runtime`. Pas encore rencontré uniquement parce que l'image runtime n'a jamais été buildée (le workflow actuel utilise le container dev).

**Conseil** : ajouter des exceptions en fin de `.dockerignore` :
```
!build/bin/MicroscopeIBOM
!docker/entrypoint.sh
```
(ou, plus propre à terme : builder le binaire dans un stage Docker dédié et le `COPY --from=`, ce qui rend l'image runtime reproductible sans dépendre d'un build local préalable).

### 1.3 🟠 L'image "runtime minimale" ne l'est pas

**Constat** : `runtime.Dockerfile` part de `FROM microscope-ibom:base`, qui contient `build-essential`, `cmake`, `git`, `ninja`, les headers `-dev` Qt/OpenGL, etc. L'image "runtime" pèsera quasiment autant que l'image dev (plusieurs Go au-dessus du strict nécessaire).

**Conseil** (quand la Phase runtime deviendra réelle) : faire un vrai stage final `FROM nvcr.io/nvidia/l4t-jetpack:r36.4.0` qui n'installe que les paquets *runtime* (libqt6*, pas qt6-*-dev) + `COPY --from=base /usr/local/lib`. Pas urgent tant que le workflow est "tout dans dev".

### 1.4 🟠 `restart: unless-stopped` sur le service `runtime` (app GUI)

**Constat** : si X n'est pas encore prêt au boot (ou que la caméra manque), l'app sort en erreur → Docker la relance en boucle indéfiniment, sans backoff visible pour l'utilisateur.

**Conseil** : `restart: on-failure:3` + un `healthcheck`, ou gérer le démarrage par un service systemd utilisateur sur l'hôte (qui attend `graphical.target`). À trancher quand le mode kiosque sera mis en place.

### 1.5 🟡 `group_add` par noms de groupes : fragile, et inutile tant qu'on tourne en root

**Constat** : `group_add: video/input/plugdev` résout les noms dans le `/etc/group` **du container**, pas de l'hôte. Si le GID `input` du container (créé par le paquet `udev`) diffère du GID propriétaire de `/dev/input/*` côté Jetson, l'accès échoue silencieusement. Aujourd'hui ça marche car les containers tournent en **root** (root ignore les permissions de groupe) — les `group_add` sont donc actuellement décoratifs.

**Conseil** : deux options cohérentes :
- court terme : laisser tel quel mais le savoir (root ⇒ accès devices OK) ;
- moyen terme (recommandé pour le mode kiosque) : créer un user non-root dans l'image et passer les GID **numériques** de l'hôte : `group_add: ["${VIDEO_GID:-44}", "${INPUT_GID:-104}"]` alimentés par un `.env` généré (`getent group video input plugdev` sur l'hôte).

### 1.6 🟡 `QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/touch0` (service runtime)

**Constat** : sous `QT_QPA_PLATFORM=xcb`, le serveur X gère déjà le tactile ; charger en plus le plugin `evdevtouch` risque des **événements tactiles en double**. De plus `/dev/input/touch0` n'existe généralement pas (c'est `/dev/input/eventN` ou un lien `by-id/`).

**Conseil** : supprimer ces deux variables tant que l'app tourne sous X11. Elles n'ont de sens qu'en mode EGLFS/KMS sans serveur X (option à considérer d'ailleurs pour le kiosque final : `QT_QPA_PLATFORM=eglfs` = pas de bureau du tout, démarrage plus rapide, rendu direct).

### 1.7 🟡 `entrypoint.sh` : check GPU et flag engines inopérants

- `nvidia-smi` sur Jetson/L4T est soit absent soit très limité (iGPU Tegra) → le check risque un faux warning permanent. Remplacer par un test de `/dev/nvgpu` ou un mini-probe `cudaGetDeviceCount` (le binaire le log déjà au démarrage).
- Le bloc "génération engines TRT" ne fait qu'un `echo` et le `touch .engines_built` est commenté → message "peut prendre 5-15 min" à **chaque** démarrage dès qu'un modèle existera. À câbler réellement quand `--build-engines` existera, ou supprimer le bloc en attendant.

### 1.8 🟢 Versionner les tags d'images

**Constat** : `microscope-ibom:base` est un tag mouvant ; un rebuild involontaire (changement de `base.Dockerfile`, prune) coûte ~90-120 min.

**Conseil** : taguer aussi par date/commit après chaque build réussi (`docker tag microscope-ibom:base microscope-ibom:base-2026-06-10`) et noter le tag dans le journal de session. Filet de sécurité quasi gratuit.

---

## 2. Scripts

### 2.1 🟠 `run_local_gui.sh` : `--force-recreate` systématique du container dev

**Constat** : chaque lancement de la GUI **détruit et recrée** le container dev — donc tue un build/une session tmux en cours dedans, et fait perdre l'état du shell.

**Conseil** : enlever `--force-recreate`. `docker compose up -d` recrée déjà automatiquement le container quand la config (fichiers compose, devices) a changé ; quand rien n'a changé, il réutilise le container existant.

### 2.2 🟡 X11 : cookie xauth vide + `xhost +local:docker`

**Constat** : `/tmp/.docker.xauth` est créé **vide** ; l'accès X ne fonctionne que grâce à `xhost +local:` qui autorise *tous* les process locaux. Acceptable sur une machine d'atelier mono-utilisateur, mais fragile (si `xhost` échoue, rien ne marche) et pas propre.

**Conseil** : peupler réellement le cookie :
```bash
xauth nlist "$DISPLAY" | sed -e 's/^..../ffff/' | xauth -f /tmp/.docker.xauth nmerge -
```
puis le `xhost` devient optionnel.

### 2.3 🟢 Divers scripts

- `bootstrap_jetson.sh` : le test réseau `ping github.com` échoue si l'ICMP est filtré → préférer `curl -fsI https://github.com` .
- `run_local_gui.sh` mode debug : `ASAN_OPTIONS` n'a d'effet que sur un binaire `build-debug/` (ASAN) mais le script lance toujours `build/bin/` → soit pointer vers `build-debug/bin` en mode debug, soit retirer la variable.
- `build_jetson.sh` : préférer `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` au hack `PATH=/usr/lib/ccache` (plus fiable avec Ninja, et visible dans le cache CMake).

---

## 3. Code C++ (spécifique Jetson)

### 3.1 🔴 Caméra : demander explicitement le format MJPG (`CameraCapture.cpp:154-157`)

**Constat** : aucune demande de FOURCC. En YUYV (défaut fréquent en V4L2), une caméra USB 2.0 ne peut **pas** fournir 1920×1080@30 (bande passante insuffisante) — le driver retombe silencieusement à 5-10 fps. C'est le risque n°1 de "ça rame" au branchement de la caméra microscope.

**Conseil** : avant de fixer largeur/hauteur :
```cpp
cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
```
puis logger le FOURCC **réel** retourné (`cap.get(cv::CAP_PROP_FOURCC)`) à côté des width/height déjà loggés. Vérifier les formats supportés avec `v4l2-ctl --list-formats-ext` dans le container.

### 3.2 🔴 Persistance config/calibration cassée en Docker

**Constat** : trois chemins de persistance **incohérents**, dont aucun ne correspond aux volumes montés par `compose.yml` :

| Donnée | Code | Chemin effectif dans le container | Persisté ? |
|---|---|---|---|
| `config.json` | `Config.cpp:24-29` (getenv HOME) | `/root/.config/MicroscopeIBOM/` | ❌ perdu au recreate (service runtime) |
| `calibration.yml` | `Application.cpp:183` (`QStandardPaths::AppDataLocation`) | `/root/.local/share/MicroscopeIBOM/` | ❌ idem |
| Volume prévu | `compose.yml` | `/opt/microscope-ibom/config` | jamais utilisé par le code |
| Cache TRT | `InferenceEngine.cpp:35` (`"trt_cache"` relatif) | `$CWD/trt_cache` | ❌ le volume monte `…/tensorrt-cache` |

Côté **dev** ça passe inaperçu (le container persiste), mais au premier `--force-recreate` ou en runtime, config + calibration sont perdues et les engines TRT seront recompilés (5-15 min) à chaque recreate.

**Conseil** : unifier sur une seule racine surchargée par variable d'environnement, par ex. `IBOM_DATA_DIR` (défaut : comportement actuel) :
- `Config.cpp` et `Application.cpp` lisent `IBOM_DATA_DIR` s'il est défini ;
- `InferenceEngine.cpp` : cache TRT sous `IBOM_DATA_DIR/tensorrt-cache` (chemin absolu) ;
- `compose.yml` : `environment: - IBOM_DATA_DIR=/opt/microscope-ibom/data` + un seul volume `../data:/opt/microscope-ibom/data`.
C'est la modification code la plus rentable avant de brancher la caméra et l'IA.

### 3.3 🟡 `-march=native` + `-flto` (CompilerFlags, Release)

**Constat** : très bien tant que le build se fait **sur** l'Orin. Mais le binaire n'est pas portable vers un autre Jetson (Nano/NX, cœurs différents), et `-march=native` rend les builds non reproductibles entre machines.

**Conseil** : pour l'Orin, expliciter `-mcpu=cortex-a78ae` (équivalent perf, reproductible), ou au minimum documenter la contrainte dans `docker/README.md`. Priorité basse tant qu'il n'y a qu'un seul Jetson.

### 3.4 🟡 TrackingWorker : CPU-only — c'est OK, mais deux pistes faciles

Le pipeline ORB+RANSAC CPU avec downscale 0.5× et throttle 200 ms est un choix pragmatique sain (ORB n'existe pas en CUDA, le gain GPU serait marginal). Deux améliorations à faible coût si le besoin apparaît :
- `cv::cuda::cvtColor` + `cv::cuda::resize` pour le pré-traitement (OpenCV est déjà compilé avec CUDA dans l'image base) — surtout intéressant en mémoire unifiée (pas de copie H↔D sur Orin) ;
- allouer les `cv::Mat` temporaires de `TrackingWorker` avec l'`UnifiedAllocator` (déjà utilisé par `CameraCapture`) pour préparer ce passage.
À mesurer d'abord avec les timings spdlog déjà en place (`prep/detect/match/homog`) — ne pas optimiser à l'aveugle.

### 3.5 🟡 Tests : `TrackingWorker` non couvert

**Constat** : 5 suites de tests (allocator, parser, homography, inference, component matching) mais rien sur `TrackingWorker::processFrame()` alors que c'est le composant le plus sensible aux différences OpenCV 4.10 Linux vs 4.12 Windows (cf. erreurs #13, #15/#16 du CLAUDE.md).

**Conseil** : un `test_tracking_worker.cpp` qui génère une image synthétique (damier + texture), la transforme par une homographie connue, et vérifie que la matrice retrouvée est proche. Pas besoin de Qt event loop : appeler `processFrame()` directement.

### 3.6 🟢 Résidus Windows (Phase 1c, déjà planifiée)

L'audit confirme que c'est propre : 8 fichiers avec `#ifdef` bien ségrégés (main.cpp, CameraCapture, Config, InferenceEngine, SettingsDialog), rien de Windows ne se compile sous Linux. Reste à supprimer quand `windows-legacy` sera considérée définitive : `build_windows.bat`, `scripts/install_prerequisites.bat`, les chemins `C:\Qt\…` dans `CMakeLists.txt:238-254`. Aucune urgence.

### 3.7 🟢 Hygiène générale : RAS

Aucune fuite détectée (smart pointers + parentage Qt systématiques), aucun `system()`/`popen()`, 69 blocs try/catch aux endroits critiques, 3 TODO seulement (OCR/Voice/arcs SVG — modules non instanciés). Bon état.

---

## 4. Process & documentation

### 4.1 🟠 Deux commits pushés sans mise à jour du journal de session

`1316f33` (scripts VNC/GUI locaux) et `a022d46` (compose.local.yml) ne figurent pas dans `JETSON_SESSION_LOG.md`, contrairement à la règle stricte du CLAUDE.md. Corrigé dans cette session (entrée 2026-06-10 rétro-documentant ces commits). Rappel : le journal fait partie du même push que le travail.

### 4.2 🟢 CI minimale

Même sans runner ARM64, un workflow GitHub Actions x86 peut déjà attraper beaucoup : check `clang-format`, build des cibles **sans** CUDA/TensorRT (`-DIBOM_ENABLE_TENSORRT=OFF`) sur Ubuntu 22.04 + Qt6 apt (mêmes versions que le Jetson !), et exécution de `test_ibom_parser`/`test_homography`/`test_component_matching` qui ne dépendent pas du GPU. Ça protège la branche `main` entre deux sessions Jetson.

### 4.3 🟢 Qt 6.2 (apt Jammy) vs habitudes Qt 6.8

Le container fournit Qt 6.2 LTS — ça marche, mais attention à ne pas introduire d'API ≥ 6.3 lors de futures sessions (elles compileraient sous Windows 6.8 mais pas sur Jetson). Une CI x86 sur Qt 6.2 (point 4.2) attrape exactement ça. La vraie montée de version viendra avec JP7/Ubuntu 24.04 (Qt 6.4+).

---

## 5. Priorisation

| Prio | Réf | Action | Effort | Impact |
|------|-----|--------|--------|--------|
| 🔴 1 | 3.2 | Unifier persistance config/calibration/cache TRT (`IBOM_DATA_DIR`) et aligner les volumes compose | ~1 session | Évite perte de config/calibration + recompilation engines à chaque recreate |
| 🔴 2 | 3.1 | Demander FOURCC MJPG + logger le format réel | ~30 min | Conditionne le 1080p@30 dès le branchement caméra |
| 🔴 3 | 1.2 | Fix `.dockerignore` vs `runtime.Dockerfile` | ~10 min | Débloque le futur build runtime |
| 🟠 4 | 2.1 | Retirer `--force-recreate` de `run_local_gui.sh` | ~5 min | Confort quotidien immédiat |
| 🟠 5 | 1.6 | Retirer `QT_QPA_GENERIC_PLUGINS`/evdevtouch sous X11 | ~5 min | Évite le double-touch au branchement du Minix SF16T |
| 🟠 6 | 3.5 | `test_tracking_worker.cpp` synthétique | ~1-2 h | Sécurise le composant le plus fragile du portage |
| 🟠 7 | 1.4 | Politique restart/healthcheck du service runtime | ~30 min | À faire avec le mode kiosque |
| 🟡 8 | 1.7 | entrypoint : check GPU adapté Tegra, bloc engines | ~20 min | Moins de faux warnings |
| 🟡 9 | 2.2 | Cookie xauth réel | ~15 min | Robustesse X11 |
| 🟡 10 | 4.2 | CI GitHub Actions x86 (format + build no-GPU + tests) | ~1 session | Filet de sécurité entre sessions |
| 🟢 11 | 1.3, 1.5, 1.8, 3.3, 3.6 | Image runtime réellement minimale, user non-root + GID numériques, tags datés, `-mcpu`, nettoyage Windows | au fil de l'eau | Hygiène long terme |

---

*Document généré suite à l'analyse du 2026-06-10 (branche `claude/dreamy-cori-oec93c`). Le fix 1.1 (doublons group_add) est appliqué dans le même commit ; tout le reste est à l'état de recommandation.*
