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
