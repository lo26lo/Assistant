# Docker — MicroscopeIBOM sur Jetson AGX Orin 32GB

Stack Docker pour développer et déployer MicroscopeIBOM sur Jetson AGX Orin avec **JetPack 7.2** (L4T r39.2, Ubuntu 24.04, CUDA 13.2, TensorRT 10.16).

Voir [../docs/JETSON_MIGRATION.md](../docs/JETSON_MIGRATION.md) pour le plan global et [../JETSON_MIGRATION_JP72.md](../JETSON_MIGRATION_JP72.md) pour la migration JP6.2 → 7.2.

> Base conteneur = `nvcr.io/nvidia/tensorrt:26.05-py3` (arm64/SBSA) : `l4t-jetpack`
> n'existe plus sur NGC en r39/JP7.

## ⚡ Setup en une commande (recommandé)

Sur un Jetson vierge avec JetPack 7.2 flashé (driver R595 minimum) :

```bash
curl -fsSL https://raw.githubusercontent.com/lo26lo/Assistant/main/scripts/bootstrap_jetson.sh | bash
```

Le script [bootstrap_jetson.sh](../scripts/bootstrap_jetson.sh) fait tout :
1. Vérifie l'environnement Jetson
2. Active le mode performance MAXN
3. Installe Docker + nvidia-container-toolkit (si manquants)
4. Clone le repo dans `~/Assistant-git`
5. Configure les règles udev RealSense
6. Build les images `microscope-ibom:base` et `:dev` (~2h cumulé)
7. Affiche les instructions pour la suite

Le script est **idempotent** (relançable sans casser l'existant). Log dans `/tmp/microscope-ibom-bootstrap.log`.

Override possibles :
```bash
REPO_DIR=/data/Assistant-git bash bootstrap_jetson.sh
TEST_IMAGE=nvcr.io/nvidia/tensorrt:26.05-py3 bash bootstrap_jetson.sh  # image test runtime nvidia
SKIP_BUILD=1 bash bootstrap_jetson.sh   # juste setup Docker, pas de build
```

## Prérequis sur l'hôte Jetson (manuel, si tu veux pas le bootstrap)

```bash
# Mode performance maximum
sudo nvpmodel -m 0 && sudo jetson_clocks

# Docker + nvidia-container-toolkit (normalement présents en JP7.2)
sudo apt install docker.io docker-compose-v2 nvidia-container-toolkit
sudo usermod -aG docker $USER
sudo systemctl restart docker

# Reconnexion shell pour appliquer le groupe docker
newgrp docker
```

Test que le runtime nvidia fonctionne :
```bash
docker run --runtime nvidia --rm nvcr.io/nvidia/tensorrt:26.05-py3 nvidia-smi
```

## Build des images

L'ordre est important : `base` doit être construite avant `dev` et `runtime`.

```bash
cd ~/Assistant-git   # ou ton chemin de clone

# 1. Base (~90-120 min première fois — compilation OpenCV from source)
docker compose -f docker/compose.yml build base

# 2. Dev
docker compose -f docker/compose.yml build dev

# 3. Runtime (à faire APRÈS avoir compilé le binaire dans dev)
# docker compose -f docker/compose.yml build runtime
```

## Workflow développement

### Quel script utiliser ? (3 entrées, ne pas confondre)

| Script | Donne | Caméra `/dev/video*` | Quand l'utiliser |
|--------|-------|:--:|------------------|
| `bash scripts/run_dev_shell.sh` | **shell dev** | ✅ oui | **Cas général** : builder, debugger, lancer le binaire à la main avec la caméra |
| `bash scripts/run_local_gui.sh` | lance le binaire directement | ✅ oui | Juste démarrer l'app (pas de shell) |
| `bash docker/run-dev.sh` | shell dev | ❌ **non** | Build pur sans caméra (legacy) |

> ⚠️ **Important** : `docker/run-dev.sh` n'utilise que `compose.yml` → **pas** de mapping
> `/dev/video*`, donc l'app y logue « Found 0 camera(s) ». Pour avoir la caméra dans
> un shell dev, utiliser **`scripts/run_dev_shell.sh`** (il génère l'override caméra
> dynamique + monte X11, comme `run_local_gui.sh`, mais te dépose dans `bash`).

### Démarrer un shell dev avec caméra (recommandé)
```bash
bash scripts/run_dev_shell.sh
```

Tu te retrouves dans le container avec :
- Code source monté en `/opt/microscope-ibom` (édition live depuis l'hôte)
- GPU + **caméras** (`/dev/video*` détectées au lancement) + écran tactile + X11
- Outils : gcc-13, cmake, ninja, gdb, ccache, vcpkg

Depuis ce shell : `bash scripts/build_jetson.sh` puis `./build/bin/MicroscopeIBOM`
(la caméra est accessible). Brancher la caméra **avant** de lancer le script
(le container est recréé avec les `/dev/video*` présents).

### Compiler le projet (dans le container dev)
```bash
bash scripts/build_jetson.sh           # Release
bash scripts/build_jetson.sh debug     # Debug + ASAN
```

Le binaire est généré dans `build/bin/MicroscopeIBOM`.

### Lancer l'app depuis le container dev
```bash
./build/bin/MicroscopeIBOM
```

L'interface s'affiche sur l'écran connecté à l'hôte (Minix SF16T).

### Sortir / arrêter
- `Ctrl+D` ou `exit` → sort du shell, container reste actif en arrière-plan
- `docker compose -f docker/compose.yml down` → arrête le container
- `docker compose -f docker/compose.yml down -v` → arrête + supprime les volumes (cache)

## Workflow runtime (production)

Une fois le binaire compilé :

```bash
docker compose -f docker/compose.yml build runtime
docker compose -f docker/compose.yml up runtime
```

L'image runtime est minimale et autostart au boot si tu actives `restart: unless-stopped` (déjà dans compose.yml).

## Configuration matérielle

### Écran tactile Minix SF16T
1. Brancher en USB-C
2. Identifier le device touch :
   ```bash
   sudo libinput list-devices | grep -B1 -A5 -i touch
   ```
3. Créer la règle udev sur l'**hôte** (pas dans le container) :
   ```bash
   # /etc/udev/rules.d/99-minix-touch.rules
   SUBSYSTEM=="input", ATTRS{name}=="*ILITEK*", SYMLINK+="input/touch0"
   ```
4. Recharger : `sudo udevadm control --reload-rules && sudo udevadm trigger`
5. Vérifier : `ls -l /dev/input/touch0` doit pointer vers `eventX`

### Caméra microscope
- Branchement USB → `/dev/video0`
- Vérifier formats : `v4l2-ctl -d /dev/video0 --list-formats-ext`
- Si plusieurs caméras, ajuster `compose.yml` (`devices:`)

### RealSense D405 (futur)
1. Brancher en USB 3.0
2. Règles udev déjà copiées dans la base image — copier sur l'hôte aussi :
   ```bash
   docker run --rm microscope-ibom:base cat /etc/udev/rules.d/99-realsense-libusb.rules \
       | sudo tee /etc/udev/rules.d/99-realsense-libusb.rules
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```
3. Tester : `docker compose ... exec dev rs-enumerate-devices` (utilitaire librealsense)

## Migration JetPack 6.2 → 7.2

Plan détaillé (reflash, sauvegarde NAS, points de vigilance) : [../JETSON_MIGRATION_JP72.md](../JETSON_MIGRATION_JP72.md).
Scripts à la racine : `jetson-backup.sh` (pré-reflash → NAS), `jetson-restore.sh` (post-reflash), `jetson-rebuild.sh` (remise en route).

Résumé de la bascule côté Docker (déjà appliquée dans ces fichiers) :

```bash
# 1. Sauvegarde vers le NAS puis reflash JP7.2 (SDK Manager, driver R595 min)
bash jetson-backup.sh        # sur l'ancien systeme, avant flash

# 2. Restauration + re-install Docker/NVIDIA toolkit (cf. prérequis ci-dessus)
bash jetson-restore.sh NAS

# 3. Base conteneur = tensorrt:26.05-py3 (déjà dans compose.yml → args: BASE_IMAGE)
#    (remplace l4t-jetpack:r36.4.0, retiré de NGC en r39)

# 4. Rebuild
docker compose -f docker/compose.yml build --no-cache base dev

# 5. Vider le cache TRT (engines incompatibles entre versions de TensorRT)
rm -rf data/tensorrt-cache/*

# 6. Lancer (engines régénérés automatiquement au premier appel)
docker compose -f docker/compose.yml up runtime
```

## Troubleshooting

| Symptôme | Solution |
|----------|----------|
| `cv2.VideoCapture(0)` retourne `False` | Vérifier `group_add: video`, `devices: /dev/video0` dans compose |
| Touch ne réagit pas | Vérifier que `/dev/input/touch0` existe sur l'hôte (udev) et `QT_QPA_GENERIC_PLUGINS` |
| GUI noir | `xhost +local:docker`, vérifier `DISPLAY` env, volume `/tmp/.X11-unix` monté |
| `nvidia-smi` échoue dans container | `runtime: nvidia` manquant dans compose.yml |
| Build OOM (Out Of Memory) | `JOBS=4 bash scripts/build_jetson.sh`, ou ajouter swap NVMe 32GB |
| Engines TRT plantent | `rm -rf tensorrt-cache/*` puis relancer |
| RealSense non détectée | Règles udev manquantes sur **l'hôte** (pas le container) |
| Perfs effondrées après 5 min | Throttling thermique : vérifier ventilateur, `nvpmodel -m 0` |
| `docker compose` plante "context canceled" | Augmenter timeout : `COMPOSE_HTTP_TIMEOUT=300 docker compose ...` |

## Tailles d'images attendues

| Image | Taille | Build initial |
|-------|--------|---------------|
| `microscope-ibom:base` | ~5 GB | 90-120 min |
| `microscope-ibom:dev` | ~8 GB | +10 min après base |
| `microscope-ibom:runtime` | ~3 GB | +5 min après base |

## Ajustements possibles

### Skip vcpkg dans dev (si tout est satisfait par apt)
Dans `docker/compose.yml`, service `dev` :
```yaml
build:
  args:
    INSTALL_VCPKG: "false"
```

### Changer la version OpenCV / RealSense
```yaml
build:
  args:
    OPENCV_VERSION: 4.11.0
    REALSENSE_VERSION: v2.56.0
```

### Désactiver libtbb (si problème de version)
Modifier `base.Dockerfile` : commenter `libtbb-dev`, retirer `WITH_TBB=ON` du cmake OpenCV.
