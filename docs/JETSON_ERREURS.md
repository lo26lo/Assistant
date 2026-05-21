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
| 4 | 2026-05-21 | apt / Qt6 base.Dockerfile | ✅ RÉSOLU | [`qt6-virtualkeyboard` n'est qu'un nom de paquet source sur Jammy](#erreur-4--qt6-virtualkeyboard-nest-quun-nom-de-paquet-source-sur-jammy) |
| 3 | 2026-05-21 | Docker / kernel Tegra | ✅ RÉSOLU | [Docker 29.x sur JP6.2 — `iptable_raw` manquant dans le kernel Tegra](#erreur-3--docker-29x-sur-jp62--iptable_raw-manquant-dans-le-kernel-tegra) |
| 2 | 2026-05-21 | Docker / image base | ✅ RÉSOLU | [Repo `dustynv/l4t-jetpack` n'existe pas sur Docker Hub](#erreur-2--repo-dustynvl4t-jetpack-nexiste-pas-sur-docker-hub) |
| 1 | 2026-05-08 | ONNX Runtime / apt ARM64 | 📝 INFO (anticipé) | [libonnxruntime-dev absent en apt Ubuntu 22.04 ARM64](#erreur-1--libonnxruntime-dev-absent-en-apt-ubuntu-2204-arm64) |

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

**Date :** 2026-05-08
**Composant :** ONNX Runtime / build base.Dockerfile
**Statut :** 📝 INFO (anticipé, pas encore observé en run)
**Référence session :** [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) session 2026-05-08 (suite)

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
