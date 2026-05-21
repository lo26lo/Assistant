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

## État actuel — au 2026-05-21 (fin de session)

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
