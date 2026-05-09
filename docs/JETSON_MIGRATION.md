# Migration vers Jetson AGX Orin 32GB — Plan complet

**Date de rédaction :** 2026-05-08
**Cible matérielle :** Seeed reComputer J401 (Jetson AGX Orin 32GB)
**JetPack actuel :** 6.2 (L4T R36.4, Ubuntu 22.04)
**JetPack futur :** 7.x (migration prévue dès compatibilité Seeed)
**Auteur du plan :** session de planification du 2026-05-08

---

## 1. Vue d'ensemble

### Contexte
Le projet **MicroscopeIBOM** (inspection PCB avec overlay iBOM IA) a été développé sur Windows avec un stack cross-platform (Qt6, OpenCV, ONNX Runtime, TensorRT optionnel). L'objectif est de le porter sur **Jetson AGX Orin 32GB** dans un container Docker, en utilisant l'écosystème [dusty-nv/jetson-containers](https://github.com/dusty-nv/jetson-containers) pour ne pas polluer l'OS hôte L4T et pour faciliter la future migration JP6→JP7.

### Contraintes utilisateur
| # | Contrainte | Impact |
|---|------------|--------|
| 1 | 1 caméra microscope USB + RealSense D405 (futur) | V4L2 + librealsense2 |
| 2 | Ne rien perdre vs version actuelle (perfs + features) | Phases 1+2 obligatoires |
| 3 | 1 seul Jetson déployé (atelier perso) | Pas de provisioning multi-machine |
| 4 | Écran tactile **Minix SF16T** + accès réseau | Qt touch + RemoteView conservé |
| 5 | Docker (ne pas polluer la VM hôte) | Architecture en couches |
| 6 | JP 6.2 (Seeed) → JP 7.x plus tard | Migration via rebuild image base |
| 7 | Utiliser `dusty-nv/jetson-containers` | Images ARM64 pré-buildées |

### Décisions stratégiques
- **Refactor niveau "Medium"** (selon notre échelle Light/Medium/Heavy) — le niveau "Heavy" (libcamera + Wayland natif) n'est pas justifié vu le scope.
- **Code C++ reste cross-platform-friendly** au cas où on relance Windows un jour, mais toutes les compilations passent désormais par Docker Linux ARM64.
- **Branche `windows-legacy` + tag `v0.1.0-windows-final`** archivent l'état Windows fonctionnel.

---

## 2. Stratégie d'archivage de la version Windows

### Ce qui a été fait (2026-05-08)
```bash
git tag -a v0.1.0-windows-final -m "Dernière version stable Windows..."
git branch windows-legacy
```

**Statut actuel :** archive **locale uniquement**. Pour la rendre permanente :
```bash
git push origin v0.1.0-windows-final
git push origin windows-legacy
```

### Comment reprendre la version Windows plus tard
```bash
git checkout windows-legacy
# OU
git checkout v0.1.0-windows-final
```

### Politique de maintenance de la branche `windows-legacy`
- **Aucun développement actif** sur `windows-legacy` une fois la migration Jetson lancée.
- Si un bug critique est découvert sur Windows et nécessite un fix : faire le fix sur `main` (Jetson), puis cherry-pick sur `windows-legacy`.
- La branche `windows-legacy` reste **gelée à v0.1.0** sauf décision explicite de redémarrer le support Windows.

### Ce qu'on garde lisible dans `main` après refactor
- Les `#ifdef IBOM_PLATFORM_WINDOWS` sont **supprimés** (simplification).
- La logique platform-specific Windows est conservée dans l'historique git de `windows-legacy`.
- Le fichier [build_windows.bat](../build_windows.bat) est **déplacé** vers `legacy/build_windows.bat.archived` ou supprimé (consultable via `git show v0.1.0-windows-final:build_windows.bat`).

---

## 3. Architecture Docker recommandée

### Schéma en couches
```
┌──────────────────────────────────────────────────────────────────┐
│  microscope-ibom:runtime    ~3 GB                                │
│  Binaire compilé + libs runtime + entrypoint                     │
│  Lancé au démarrage manuel par l'utilisateur (pas systemd auto)  │
├──────────────────────────────────────────────────────────────────┤
│  microscope-ibom:dev        ~8 GB                                │
│  + vcpkg, build-essential, gdb, ninja, valgrind, clang-format    │
│  Utilisé pour développer, builder, débugger                      │
├──────────────────────────────────────────────────────────────────┤
│  microscope-ibom:base       ~5 GB                                │
│  + Qt6, OpenCV (CUDA), TensorRT, librealsense2, spdlog, ZXing    │
│  Reconstruit rarement (1× par version JetPack)                   │
├──────────────────────────────────────────────────────────────────┤
│  FROM dustynv/l4t-jetpack:r36.4.0                                │
│  CUDA 12.6, cuDNN 8.9, TensorRT 10.3, L4T R36.4                  │
│  Géré par jetson-containers (déjà testé NVIDIA)                  │
└──────────────────────────────────────────────────────────────────┘
```

### Workflow type
1. **Setup initial** (1 fois) : build de `:base` (~2h sur Jetson, ARM64 from source).
2. **Dev quotidien** : `docker compose up dev`, code source monté en volume, recompilation incrémentale dans le conteneur.
3. **Test runtime** : `docker compose up runtime` lance l'app finale.
4. **Migration JP7** : modifier `FROM` dans `base.Dockerfile`, rebuild → tout le reste suit.

---

## 4. Configuration matérielle

### 4.1 Écran tactile Minix SF16T

**Caractéristiques** :
- 15.6" IPS 1920×1080 60Hz
- Touch capacitif 10 points
- Connexion USB-C (data + power) ou HDMI + USB séparés
- Standard HID-multitouch — **aucun driver propriétaire requis sous Linux**

**Configuration Linux** :
1. Brancher l'écran en USB-C (display + touch dans un câble).
2. Identifier le device touch :
   ```bash
   ls /dev/input/by-id/ | grep -i touch
   # ou
   sudo evtest  # liste tous les devices, repérer "Minix" ou "ILITEK"
   ```
3. Le device apparaîtra typiquement comme `/dev/input/eventX` (X variable, 5-10 selon l'ordre USB).
4. **Persistance du nom** via udev :
   ```bash
   # /etc/udev/rules.d/99-minix-touch.rules
   SUBSYSTEM=="input", ATTRS{name}=="*ILITEK*", SYMLINK+="input/touch0"
   ```
   Ainsi le device sera toujours `/dev/input/touch0`.

**Configuration Qt6** (variables d'environnement dans le container) :
```bash
QT_QPA_PLATFORM=xcb
QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/touch0
QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=rotate=0:invertx=0:inverty=0
```

**Adaptations UI à prévoir** dans [src/gui/](../src/gui/) :
- Hit areas minimum **44 px** (recommandation accessibilité tactile).
- Boutons espacés (éviter les radio buttons collés).
- Ajout `qt6-virtualkeyboard` pour saisies (numéros de série, notes inspection).
- Tester pinch-zoom / pan dans `CameraView` via `QGestureEvent` natif.
- Pas de tooltips au survol (impossible au tactile) — remplacer par tap-and-hold ou icônes "?".

### 4.2 Caméra microscope USB

**Hypothèse** : caméra UVC standard (la majorité des microscopes USB).

**Configuration** :
- Apparaît comme `/dev/video0` (ou supérieur si plusieurs caméras).
- Tester avec `v4l2-ctl --list-devices` et `v4l2-ctl -d /dev/video0 --list-formats-ext`.
- Permissions : utilisateur dans le groupe `video` (`sudo usermod -aG video $USER`).
- Backend OpenCV : `cv::CAP_V4L2` (déjà géré dans [src/camera/CameraCapture.cpp:96](../src/camera/CameraCapture.cpp#L96)).

**Aucun changement de code requis** — la branche Linux existe déjà.

### 4.3 RealSense D405 (futur)

**Caractéristiques** :
- Caméra de profondeur courte distance (focus 7-50 cm, précision ~0.1 mm à 10 cm).
- **Idéale pour PCB** : mesure de hauteur de composants, vérification volumétrique de soudure.
- USB 3.0, alimentée USB.

**Stack logiciel** :
- **librealsense2** — packagée dans `jetson-containers/realsense` (compilée pour ARM64).
- Wrapper C++ officiel : `#include <librealsense2/rs.hpp>`.
- Apparaît comme **plusieurs `/dev/videoX`** (RGB + depth + IR).

**Règles udev (sur l'hôte)** :
```bash
# Cloner librealsense pour récupérer les règles
git clone https://github.com/IntelRealSense/librealsense.git
sudo cp librealsense/config/99-realsense-libusb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**Modules à créer (futur)** :
- `src/camera/RealSenseCapture.cpp/.h` — capture RGB + depth synchronisé
- `src/features/Depth3DInspector.cpp/.h` — vérif hauteur composants
- `src/features/StencilAlign3D.cpp` — extension de [src/features/StencilAlign.cpp](../src/features/StencilAlign.cpp) pour alignement 3D

**Le code actuel n'a rien à changer maintenant** — la D405 sera additive.

---

## 5. Choix du modèle YOLO

### Recommandation : **YOLOv8m FP16** sur GPU

**Justification** :
| Modèle | Taille | FPS Jetson AGX Orin (1080p, FP16) | Précision SMD | Verdict |
|--------|--------|-----------------------------------|---------------|---------|
| YOLOv8n | 6.2 MB | ~120 fps | Médiocre sur 0402/0201 | Trop léger |
| YOLOv8s | 21 MB | ~70 fps | Bon | OK si budget perfs serré |
| **YOLOv8m** | **49 MB** | **~40-50 fps** | **Excellent** | **✅ Recommandé** |
| YOLOv8l | 83 MB | ~22 fps | Excellent | Overkill |
| YOLOv8x | 130 MB | ~12 fps | Excellent | Trop lent |

**Pourquoi YOLOv8m** :
- 40-50 fps en FP16 sur GPU = bien au-dessus du temps réel pour inspection visuelle (10-15 fps suffisent perçus comme fluides).
- Précision suffisante pour SMD jusqu'à 0402, et acceptable sur 0201 avec dataset bien annoté.
- Laisse du budget GPU pour `OCREngine` + `SolderInspector` + overlay rendering en parallèle.
- Compatible TensorRT + DLA si optimisation INT8 plus tard.

### Alternative à considérer : YOLOv11m
Sortie en 2024, légèrement plus précis et plus rapide que YOLOv8m. À évaluer si l'écosystème Ultralytics a stabilisé son support TensorRT 10.x.

### Stratégie d'inférence multi-modèle
| Tâche | Modèle | Backend | Justification |
|-------|--------|---------|---------------|
| Détection composants | YOLOv8m | GPU FP16 | Précision critique |
| Inspection soudure | Modèle classification custom | GPU FP16 | Petite résolution (32×32 ROI) |
| OCR (refdes) | PP-OCRv4 ou EasyOCR | GPU FP16 ou DLA INT8 | Dépend du volume |
| Détection 3D (D405 futur) | Heuristique + librealsense | CPU | Pas besoin de réseau profond |

### Important : régénération des engines TensorRT
Les fichiers `.engine` ou `.trt` sont **liés à l'architecture GPU**. Un engine généré sur :
- RTX 5070 desktop (Blackwell) → **incompatible** Jetson AGX Orin (Ampere)
- JetPack 6.2 → **incompatible** JetPack 7.x (versions TRT différentes)

→ La génération des engines doit avoir lieu **au premier lancement** sur la cible, idéalement en cache local (volume Docker monté).

---

## 6. Plan de refactor par phases

### Phase 0 — Conteneurisation (3-5 jours)
**Objectif** : avoir un environnement Docker dev fonctionnel sur le Jetson.

**Livrables** :
- [ ] `docker/base.Dockerfile` (Qt6 + OpenCV CUDA + TensorRT + librealsense)
- [ ] `docker/dev.Dockerfile` (FROM base + outils dev)
- [ ] `docker/runtime.Dockerfile` (FROM base + binaire stripped)
- [ ] `docker/compose.yml` (services dev + runtime avec --runtime nvidia, devices, X11)
- [ ] `docker/run-dev.sh` (wrapper avec exports DISPLAY + xhost)
- [ ] Test : `xeyes` ou `glxgears` lancé depuis le container affiché sur Minix SF16T
- [ ] Test : caméra USB capturée depuis le container (script Python `cv2.VideoCapture(0)`)

**Critère de succès** : compiler le projet existant dans le container dev sans erreur (avec les `#ifdef WIN32` encore présents — on les enlève en Phase 1).

### Phase 1 — Portage Linux pur (1-2 jours)
**Objectif** : code propre, Linux-natif, mais comportement identique à la version Windows actuelle.

**Fichiers à modifier** :
| Fichier | Action |
|---------|--------|
| [src/main.cpp:9-25](../src/main.cpp#L9-L25) | Remplacer `SetUnhandledExceptionFilter` par handler POSIX `signal(SIGSEGV, ...)` ou suppression |
| [src/app/Config.cpp:16-30](../src/app/Config.cpp#L16-L30) | Garder uniquement la branche Linux (XDG `~/.config/MicroscopeIBOM`) |
| [src/camera/CameraCapture.cpp:93-123](../src/camera/CameraCapture.cpp#L93-L123) | Garder uniquement la branche V4L2 |
| [src/ai/InferenceEngine.cpp:68](../src/ai/InferenceEngine.cpp#L68) | Vérifier le `#ifdef _WIN32` — probablement chargement DLL TensorRT, à remplacer par `.so` ou suppression si TRT lié statiquement |
| [CMakeLists.txt:306-314](../CMakeLists.txt#L306-L314) | Supprimer les branches `WIN32`, simplifier en `IBOM_PLATFORM_LINUX` constant |
| [vcpkg.json](../vcpkg.json) | Retirer `msmf` de la feature OpenCV, retirer `onnxruntime-gpu` (TRT JetPack à la place) |
| [build_windows.bat](../build_windows.bat) | Supprimer (consultable via tag `v0.1.0-windows-final`) |
| `scripts/install_prerequisites.bat` | Supprimer (idem) |

**Nouveau fichier** : `scripts/build_jetson.sh`
```bash
#!/bin/bash
set -e
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-linux \
  -DIBOM_ENABLE_TENSORRT=ON \
  -DIBOM_ENABLE_VOICE=OFF \
  -DIBOM_ENABLE_REMOTE=ON
cmake --build build -j$(nproc)
```

**Critère de succès** : binaire qui démarre, ouvre la GUI sur Minix SF16T, capture la caméra, charge un PCB iBOM, fait l'inférence (à fps modeste, l'optim vient en Phase 2).

### Phase 2 — Mémoire unifiée (zero-copy) — 1-2 semaines
**Objectif** : exploiter la mémoire partagée CPU/GPU du Jetson pour matcher (ou dépasser) les perfs RTX 5070 desktop.

**Statut Phase 2a/b/c — livré 2026-05-09** : `FrameBuffer` (dead code) supprimé, nouvel allocateur OpenCV `UnifiedAllocator` (`cudaMallocManaged` sous `IBOM_USE_UMA_ALLOCATOR`, fallback `std::malloc`), `CameraCapture::captureLoop` capture désormais directement dans une `cv::Mat` UMA. Reste à valider runtime sur Jetson.

**Fichiers concernés** :
- ~~`src/camera/FrameBuffer.cpp/.h`~~ — supprimé (dead code, jamais consommé). Remplacé par l'approche allocator-per-Mat.
- [src/camera/UnifiedAllocator.h/.cpp](../src/camera/UnifiedAllocator.h) — nouveau, `cv::MatAllocator` basé sur `cudaMallocManaged`
- [src/camera/CameraCapture.cpp](../src/camera/CameraCapture.cpp) — capture loop branche `frame.allocator = unifiedAllocator()` (✅ fait). Activation V4L2 DMABUF restant pour Phase 2.5 (gros morceau, hors `cv::VideoCapture`).
- [src/ai/InferenceEngine.cpp](../src/ai/InferenceEngine.cpp) — supprimer copies `cv::Mat` host intermédiaires (Phase 2d, à faire quand l'inférence sera instanciée et qu'on pourra valider sur Jetson)

**Pattern cible** :
```
V4L2 (DMABUF) → CUDA EGL Stream → TensorRT → OpenGL texture (overlay)
                  └─ même mémoire physique tout du long ─┘
```

**Gain attendu** : +30 à 50% de fps sur la pipeline complète.

**Critère de succès** : benchmark `nsys profile` montrant <1 ms de copies host↔device par frame.

### Phase 3 — DLA + INT8 (optionnel, 1 semaine)
**Objectif** : décharger le détecteur principal sur DLA pour libérer le GPU.

À faire **uniquement si** la Phase 2 ne suffit pas à atteindre les fps souhaités.

**Étapes** :
1. Calibration INT8 du `ComponentDetector` sur ~500 images représentatives.
2. Configuration TensorRT : `config->setDeviceType(layer, DeviceType::kDLA); config->setDLACore(0);`
3. Fallback GPU pour les layers non supportés DLA (`config->setFlag(BuilderFlag::kGPU_FALLBACK)`).
4. Validation : précision mAP DLA-INT8 vs GPU-FP16 (acceptable si ΔmAP < 2 points).

**Gain attendu** : +20-30% de fps + GPU disponible pour OCR/heatmap en parallèle.

---

## 7. Layout du repo après refactor

```
Assistant-git/
├── docker/                          # NOUVEAU
│   ├── base.Dockerfile
│   ├── dev.Dockerfile
│   ├── runtime.Dockerfile
│   ├── compose.yml
│   ├── run-dev.sh
│   ├── entrypoint.sh
│   └── README.md
├── docs/
│   ├── JETSON_MIGRATION.md          # ← ce document
│   ├── JOURNAL_ERREURS.md
│   ├── PROJECT_PLAN.md
│   ├── PROJECT_STATE.md
│   └── REPRISE_SESSION.md
├── scripts/
│   ├── build_jetson.sh              # NOUVEAU (remplace build_windows.bat)
│   ├── install_prerequisites.sh     # MIS À JOUR pour JP6.2 + Docker
│   └── generate_calibration_pattern.py
├── src/                             # Code C++ — Linux only après Phase 1
├── tests/
├── models/
├── resources/
├── cmake/
├── CMakeLists.txt                   # Branches Windows retirées
├── vcpkg.json                       # Triplet arm64-linux
├── README.md
└── TODO.md
```

**Fichiers supprimés (consultables via `git checkout windows-legacy`)** :
- `build_windows.bat`
- `scripts/install_prerequisites.bat`

---

## 8. Configuration Docker — squelette

### 8.1 `docker/base.Dockerfile`
```dockerfile
ARG L4T_VERSION=r36.4.0
FROM dustynv/l4t-jetpack:${L4T_VERSION}

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Europe/Paris

# Outils système
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ninja-build cmake git curl wget pkg-config \
    autoconf automake libtool unzip ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Qt6 (depuis dépôts Ubuntu 22.04 — Qt 6.2 LTS)
# Pour Qt 6.5+ : à compiler depuis source (~3h ARM64)
RUN apt-get update && apt-get install -y --no-install-recommends \
    qt6-base-dev qt6-multimedia-dev qt6-tools-dev \
    libqt6opengl6-dev libqt6websockets6-dev \
    qt6-virtualkeyboard \
    && rm -rf /var/lib/apt/lists/*

# OpenCV CUDA — utiliser jetson-containers ou rebuild from source
# (le opencv système Jetson n'a PAS le module CUDA activé)
# Option A : COPY depuis image jetson-containers/opencv:cuda
# Option B : compiler ici (~1h)
COPY --from=dustynv/opencv:4.8.0-cuda-r36.4.0 /usr/local/lib /usr/local/lib
COPY --from=dustynv/opencv:4.8.0-cuda-r36.4.0 /usr/local/include /usr/local/include
RUN ldconfig

# librealsense (RealSense D405 — futur usage)
COPY --from=dustynv/realsense:r36.4.0 /usr/local/lib /usr/local/lib
COPY --from=dustynv/realsense:r36.4.0 /usr/local/include /usr/local/include
RUN ldconfig

# Dépendances applicatives
RUN apt-get update && apt-get install -y --no-install-recommends \
    libspdlog-dev libfmt-dev nlohmann-json3-dev \
    libzxing-dev libhpdf-dev catch2 \
    libv4l-dev v4l-utils \
    libusb-1.0-0-dev \
    && rm -rf /var/lib/apt/lists/*

# Variables Qt6 pour touch
ENV QT_QPA_PLATFORM=xcb
ENV QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/touch0
ENV QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=rotate=0

WORKDIR /opt/microscope-ibom
```

### 8.2 `docker/dev.Dockerfile`
```dockerfile
FROM microscope-ibom:base

RUN apt-get update && apt-get install -y --no-install-recommends \
    gdb valgrind clang-format clang-tidy \
    vim nano less htop \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# vcpkg (utile si dépendances supplémentaires non packagées en apt)
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${VCPKG_ROOT}:${PATH}"

CMD ["/bin/bash"]
```

### 8.3 `docker/runtime.Dockerfile`
```dockerfile
FROM microscope-ibom:base

# On suppose que le binaire est build via dev container et copié ici
COPY build/bin/MicroscopeIBOM /opt/microscope-ibom/MicroscopeIBOM
COPY models/ /opt/microscope-ibom/models/
COPY resources/ /opt/microscope-ibom/resources/

# Stripper les symboles pour réduire la taille
RUN strip /opt/microscope-ibom/MicroscopeIBOM

COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
```

### 8.4 `docker/compose.yml`
```yaml
services:
  dev:
    build:
      context: ..
      dockerfile: docker/dev.Dockerfile
    image: microscope-ibom:dev
    runtime: nvidia
    network_mode: host
    devices:
      - /dev/video0:/dev/video0
      - /dev/bus/usb:/dev/bus/usb
      - /dev/input:/dev/input
      - /dev/dri:/dev/dri
    environment:
      - DISPLAY=${DISPLAY:-:0}
      - QT_QPA_PLATFORM=xcb
      - QT_X11_NO_MITSHM=1
    volumes:
      - /tmp/.X11-unix:/tmp/.X11-unix:rw
      - ..:/opt/microscope-ibom:rw
      - microscope-cache:/root/.cache
    group_add:
      - video
      - plugdev
    stdin_open: true
    tty: true
    command: /bin/bash

  runtime:
    build:
      context: ..
      dockerfile: docker/runtime.Dockerfile
    image: microscope-ibom:runtime
    runtime: nvidia
    network_mode: host
    devices:
      - /dev/video0:/dev/video0
      - /dev/bus/usb:/dev/bus/usb
      - /dev/input:/dev/input
      - /dev/dri:/dev/dri
    environment:
      - DISPLAY=${DISPLAY:-:0}
      - QT_QPA_PLATFORM=xcb
      - QT_QPA_GENERIC_PLUGINS=evdevtouch:/dev/input/touch0
    volumes:
      - /tmp/.X11-unix:/tmp/.X11-unix:rw
      - ./config:/opt/microscope-ibom/config:rw
      - ./logs:/opt/microscope-ibom/logs:rw
      - ./models:/opt/microscope-ibom/models:ro
      - ./tensorrt-cache:/opt/microscope-ibom/tensorrt-cache:rw
    group_add:
      - video
      - plugdev

volumes:
  microscope-cache:
```

### 8.5 `docker/run-dev.sh`
```bash
#!/bin/bash
# Wrapper pour lancer le container dev avec X11 forwarding
xhost +local:docker
docker compose -f docker/compose.yml up -d dev
docker compose -f docker/compose.yml exec dev /bin/bash
```

### 8.6 `docker/entrypoint.sh`
```bash
#!/bin/bash
set -e

# Régénération des engines TensorRT au premier lancement si absents
if [ ! -f /opt/microscope-ibom/tensorrt-cache/component_detector.engine ]; then
    echo "[entrypoint] Génération des engines TensorRT (peut prendre 5-15 min)..."
    /opt/microscope-ibom/MicroscopeIBOM --build-engines
fi

exec /opt/microscope-ibom/MicroscopeIBOM "$@"
```

---

## 9. Migration JetPack 6.2 → 7.x — procédure

Quand JP 7.x sera officiellement compatible avec le reComputer Seeed :

1. **Backup données utilisateur** :
   ```bash
   tar czf jetson-data-backup-$(date +%F).tar.gz \
     ./config ./logs ./models ./tensorrt-cache
   ```
2. **Flash de l'hôte L4T** via SDK Manager NVIDIA (ou image Seeed pré-configurée).
3. **Réinstaller Docker + jetson-containers** sur le nouveau L4T.
4. **Modifier `docker/base.Dockerfile`** :
   ```diff
   -ARG L4T_VERSION=r36.4.0
   +ARG L4T_VERSION=r38.0.0  # exemple JP7
   ```
5. **Rebuild les 3 images** :
   ```bash
   docker compose -f docker/compose.yml build --no-cache
   ```
6. **Supprimer les engines TensorRT cachés** (incompatibles entre versions TRT) :
   ```bash
   rm -rf ./tensorrt-cache/*
   ```
7. **Lancer** : les engines seront régénérés automatiquement par `entrypoint.sh`.
8. **Tester** : caméra, GUI tactile, RemoteView, inférence.

**Code C++ à modifier : zéro** (sauf changement d'ABI Qt entre versions, peu probable sur LTS).

---

## 10. Checklist de mise en route

### Préparation matérielle
- [ ] Jetson AGX Orin 32GB Seeed reComputer reçu et démarré
- [ ] NVMe M.2 SSD installé (recommandation : 1TB, l'eMMC 64GB est insuffisant pour vcpkg + builds)
- [ ] Connexion Ethernet ou Wi-Fi configurée
- [ ] Minix SF16T branché (USB-C ou HDMI+USB)
- [ ] Caméra microscope USB connectée
- [ ] Ventilateur actif vérifié (mode MAXN nécessite refroidissement)

### Préparation logicielle
- [ ] JP 6.2 confirmé (`cat /etc/nv_tegra_release`)
- [ ] Mode performance maximum :
  ```bash
  sudo nvpmodel -m 0
  sudo jetson_clocks
  ```
- [ ] Docker installé + groupe utilisateur :
  ```bash
  sudo apt install docker.io docker-compose-v2
  sudo usermod -aG docker $USER
  ```
- [ ] NVIDIA Container Toolkit installé (normalement inclus JP6.2) :
  ```bash
  sudo apt install nvidia-container-toolkit
  sudo systemctl restart docker
  ```
- [ ] Test GPU dans Docker :
  ```bash
  docker run --runtime nvidia --rm dustynv/l4t-jetpack:r36.4.0 nvidia-smi
  ```
- [ ] `jetson-containers` cloné et testé :
  ```bash
  git clone https://github.com/dusty-nv/jetson-containers /opt/jetson-containers
  cd /opt/jetson-containers && bash install.sh
  ```

### Préparation projet
- [ ] Repo cloné sur le Jetson (NVMe, pas eMMC)
- [ ] Branche `main` checkée
- [ ] Tag `v0.1.0-windows-final` visible (`git tag`)
- [ ] Branche `windows-legacy` visible (`git branch -a`)
- [ ] Tag et branche pushés sur origin (si désiré) :
  ```bash
  git push origin v0.1.0-windows-final
  git push origin windows-legacy
  ```

### Validation Phase 0 (Docker)
- [ ] `docker/base.Dockerfile` créé et build réussi
- [ ] `docker/dev.Dockerfile` créé et build réussi
- [ ] `xeyes` lancé depuis container affiché sur Minix SF16T
- [ ] `glxinfo | grep "OpenGL renderer"` montre le GPU NVIDIA
- [ ] Caméra microscope capturée depuis container :
  ```bash
  docker compose run dev bash -c "v4l2-ctl --list-devices && python3 -c 'import cv2; cap=cv2.VideoCapture(0); print(cap.read()[1].shape)'"
  ```

### Validation Phase 1 (portage Linux)
- [ ] Tous les `#ifdef IBOM_PLATFORM_WINDOWS` retirés ou simplifiés
- [ ] `build_windows.bat` et `install_prerequisites.bat` supprimés
- [ ] `scripts/build_jetson.sh` créé et exécutable
- [ ] Build complet sans erreur : `docker compose run dev bash scripts/build_jetson.sh`
- [ ] Lancement GUI sur Minix SF16T avec touch fonctionnel
- [ ] Capture caméra dans GUI fonctionnelle
- [ ] Chargement iBOM JSON fonctionnel
- [ ] Inférence sur une frame statique fonctionnelle (ignorer les fps pour l'instant)

### Validation Phase 2 (mémoire unifiée)
- [ ] `FrameBuffer` refactoré avec `cudaMallocManaged`
- [ ] `nsys profile` montre <1 ms copies host↔device par frame
- [ ] Benchmark fps : ≥ valeur baseline x1.3

---

## 11. Annexes

### 11.1 Commandes utiles Jetson

```bash
# Statut GPU + thermal
sudo tegrastats

# Mode puissance (0=MAXN, 1=15W, 2=30W, 3=50W)
sudo nvpmodel -q
sudo nvpmodel -m 0

# Boost CPU/GPU/EMC clocks au max
sudo jetson_clocks

# Version JetPack
sudo apt-cache show nvidia-jetpack | grep Version

# Liste devices V4L2
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext

# Liste devices touch
sudo libinput list-devices | grep -A5 -i touch

# Test touch en live
sudo evtest /dev/input/event5  # remplacer eventX par le bon
```

### 11.2 Troubleshooting

| Symptôme | Cause probable | Solution |
|----------|----------------|----------|
| `cv2.VideoCapture(0)` retourne `False` dans container | Permissions `/dev/video0` | Ajouter `group_add: [video]` dans compose.yml |
| Touch screen ne réagit pas dans Qt | Mauvais event device | `evtest` pour identifier, ajuster `QT_QPA_GENERIC_PLUGINS` |
| GUI noir / pas d'affichage | X11 socket non monté | `xhost +local:docker` + volume `/tmp/.X11-unix` |
| `nvidia-smi` ne fonctionne pas dans container | `--runtime nvidia` manquant | Vérifier compose.yml |
| Build OOM (out of memory) | `ninja -j8` trop agressif sur ARM | `ninja -j4` ou ajouter swap NVMe 32GB |
| Engines TensorRT plantent au lancement | Engine généré sur autre GPU/JetPack | Supprimer `tensorrt-cache/*.engine`, relancer |
| RealSense D405 non détectée | Règles udev manquantes | Installer `99-realsense-libusb.rules` sur l'hôte |
| Perfs effondrées après 5 min | Throttling thermique | Vérifier ventilateur, mode `nvpmodel -m 0`, environnement <30°C |

### 11.3 Ressources externes

- [dusty-nv/jetson-containers](https://github.com/dusty-nv/jetson-containers) — référence absolue
- [JetPack 6.2 Documentation](https://docs.nvidia.com/jetson/jetpack/index.html)
- [TensorRT 10.x Developer Guide](https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/)
- [librealsense2 Cross-Compilation Jetson](https://github.com/IntelRealSense/librealsense/blob/master/doc/installation_jetson.md)
- [Seeed reComputer J401](https://wiki.seeedstudio.com/reComputer_J4012_Getting_Started/)
- [Qt 6 Touch Input Documentation](https://doc.qt.io/qt-6/qtouchevent.html)
- [Ultralytics YOLOv8 → TensorRT Export](https://docs.ultralytics.com/integrations/tensorrt/)

---

## 12. Décisions ouvertes / TODO

- [ ] **Modèle YOLO** : confirmer YOLOv8m vs YOLOv11m après benchmark sur dataset PCB réel.
- [ ] **Qt6 version** : Qt 6.2 LTS depuis apt suffit-il, ou compiler Qt 6.6+ from source ?
- [ ] **OpenCV** : utiliser image `dustynv/opencv:cuda` ou recompiler une version spécifique ?
- [ ] **Cache TensorRT** : volume Docker nommé ou bind mount ?
- [ ] **Logs** : intégrer `journald` (`sd_journal_send`) ou rester sur fichiers spdlog ?
- [ ] **Auto-démarrage** : décidé non — l'utilisateur lancera manuellement (`docker compose up runtime`).

---

**Fin du document.** Pour toute mise à jour, modifier ce fichier et committer avec un message du type `docs: jetson migration plan — <changement>`.
