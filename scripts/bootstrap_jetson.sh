#!/bin/bash
# =============================================================================
#  bootstrap_jetson.sh — Setup complet pour Jetson AGX Orin
#  Une commande pour partir d'un Jetson vierge et arriver a un container
#  dev pret a compiler MicroscopeIBOM.
#
#  Usage:
#    # Option 1 (one-liner, le plus simple):
#    curl -fsSL https://raw.githubusercontent.com/lo26lo/Assistant/main/scripts/bootstrap_jetson.sh | bash
#
#    # Option 2 (telecharger puis executer):
#    wget https://raw.githubusercontent.com/lo26lo/Assistant/main/scripts/bootstrap_jetson.sh
#    chmod +x bootstrap_jetson.sh
#    ./bootstrap_jetson.sh
#
#    # Option 3 (depuis le repo deja clone):
#    cd ~/Assistant-git
#    bash scripts/bootstrap_jetson.sh
#
#  Variables d'environnement (override possibles) :
#    REPO_URL        URL du repo (default: https://github.com/lo26lo/Assistant.git)
#    REPO_DIR        Chemin de clone (default: $HOME/Assistant-git)
#    TEST_IMAGE      Image de test du runtime nvidia (default: tensorrt:26.05-py3)
#                    (l4t-jetpack a ete retire de NGC en r39/JP7)
#    LOG_FILE        Fichier log (default: /tmp/microscope-ibom-bootstrap.log)
#    SKIP_BUILD      1 = ne pas builder les images Docker (default: 0)
#    SKIP_PERFMODE   1 = ne pas activer MAXN (default: 0)
#
#  Etapes:
#    1. Verifications prealables (Jetson, sudo, internet)
#    2. Mode performance MAXN
#    3. Docker + nvidia-container-toolkit
#    4. Clone du repo (ou git pull si deja clone)
#    5. Regles udev RealSense (pour la D405 future)
#    6. Build de l'image microscope-ibom:base (~90-120 min)
#    7. Build de l'image microscope-ibom:dev
#    8. Recap et instructions
#
#  Idempotent: peut etre relance sans casser une installation existante.
# =============================================================================

set -euo pipefail

# -----------------------------------------------------------------------------
#  Configuration
# -----------------------------------------------------------------------------
REPO_URL="${REPO_URL:-https://github.com/lo26lo/Assistant.git}"
REPO_DIR="${REPO_DIR:-$HOME/Assistant-git}"
# Image de test du runtime nvidia (JP7.2 : l4t-jetpack retire de NGC en r39).
TEST_IMAGE="${TEST_IMAGE:-nvcr.io/nvidia/tensorrt:26.05-py3}"
LOG_FILE="${LOG_FILE:-/tmp/microscope-ibom-bootstrap.log}"
SKIP_BUILD="${SKIP_BUILD:-0}"
SKIP_PERFMODE="${SKIP_PERFMODE:-0}"

# Couleurs
RED=$'\033[0;31m'
GRN=$'\033[0;32m'
YEL=$'\033[1;33m'
BLU=$'\033[1;34m'
BOLD=$'\033[1m'
NC=$'\033[0m'

# -----------------------------------------------------------------------------
#  Helpers
# -----------------------------------------------------------------------------
log()   { echo "${BLU}[bootstrap]${NC} $*" | tee -a "$LOG_FILE"; }
warn()  { echo "${YEL}[WARN]${NC} $*" | tee -a "$LOG_FILE" >&2; }
error() { echo "${RED}[ERROR]${NC} $*" | tee -a "$LOG_FILE" >&2; }
ok()    { echo "${GRN}[OK]${NC} $*" | tee -a "$LOG_FILE"; }
fatal() { error "$*"; exit 1; }

step() {
    echo "" | tee -a "$LOG_FILE"
    echo "${BOLD}=== $* ===${NC}" | tee -a "$LOG_FILE"
}

# Wrapper docker qui utilise sudo si l'user n'est pas (encore) dans le groupe docker
docker_cmd() {
    if groups | grep -q '\bdocker\b' && docker info >/dev/null 2>&1; then
        docker "$@"
    else
        sudo docker "$@"
    fi
}

# -----------------------------------------------------------------------------
#  Init log
# -----------------------------------------------------------------------------
echo "==== bootstrap_jetson.sh start at $(date -Iseconds) ====" > "$LOG_FILE"

cat <<BANNER

==============================================================================
${BOLD} MicroscopeIBOM — Bootstrap Jetson AGX Orin${NC}
 Repo:        $REPO_URL
 Destination: $REPO_DIR
 Test image:  $TEST_IMAGE
 Log:         $LOG_FILE
==============================================================================

BANNER

# =============================================================================
#  STEP 1 — Verifications prealables
# =============================================================================
step "[1/8] Verifications prealables"

if [ "$EUID" -eq 0 ]; then
    fatal "Ne pas lancer ce script en root. Utiliser un user normal — sudo sera demande au besoin."
fi

if [ ! -f /etc/nv_tegra_release ]; then
    warn "/etc/nv_tegra_release absent — pas un Jetson ?"
    if [ -t 0 ]; then
        read -rp "Continuer quand meme ? (y/N) " ans
        [[ "$ans" =~ ^[Yy]$ ]] || fatal "Abandon."
    else
        fatal "Pas de TTY pour confirmer — abandon."
    fi
else
    log "JetPack: $(head -1 /etc/nv_tegra_release)"
fi

command -v sudo >/dev/null || fatal "sudo non installe"
command -v git  >/dev/null || { log "Installation git..."; sudo apt-get update -qq && sudo apt-get install -y git; }
command -v curl >/dev/null || { log "Installation curl..."; sudo apt-get install -y curl; }

# Maintenir sudo en cache pendant tout le script
log "Demande de credentials sudo (sera mis en cache)..."
sudo -v || fatal "Impossible d'obtenir sudo"
(while true; do sudo -n true 2>/dev/null || exit; sleep 60; done) &
SUDO_KEEPALIVE_PID=$!
trap 'kill $SUDO_KEEPALIVE_PID 2>/dev/null || true' EXIT

# Connexion internet
ping -c1 -W3 github.com >/dev/null 2>&1 || fatal "Pas de connexion a github.com"

ok "Pre-verifications OK"

# =============================================================================
#  STEP 2 — Mode performance MAXN
# =============================================================================
step "[2/8] Mode performance MAXN"

if [ "$SKIP_PERFMODE" = "1" ]; then
    log "SKIP (SKIP_PERFMODE=1)"
else
    if command -v nvpmodel >/dev/null; then
        sudo nvpmodel -m 0 >>"$LOG_FILE" 2>&1 || warn "nvpmodel echec"
        ok "nvpmodel mode 0 (MAXN)"
    else
        warn "nvpmodel introuvable — skip"
    fi
    if command -v jetson_clocks >/dev/null; then
        sudo jetson_clocks >>"$LOG_FILE" 2>&1 || warn "jetson_clocks echec"
        ok "jetson_clocks active"
    else
        warn "jetson_clocks introuvable — skip"
    fi
fi

# =============================================================================
#  STEP 3 — Docker + NVIDIA Container Toolkit
# =============================================================================
step "[3/8] Docker + NVIDIA Container Toolkit"

if ! command -v docker >/dev/null; then
    log "Installation docker.io + docker-compose-v2..."
    sudo apt-get update -qq
    sudo apt-get install -y docker.io docker-compose-v2
fi

if ! sudo systemctl is-active --quiet docker; then
    log "Demarrage du daemon Docker..."
    sudo systemctl enable --now docker
fi

# Ajout au groupe docker (effet au prochain login)
if ! groups "$USER" | grep -q '\bdocker\b'; then
    log "Ajout de $USER au groupe docker..."
    sudo usermod -aG docker "$USER"
    GROUP_ADDED=1
else
    GROUP_ADDED=0
fi

# nvidia-container-toolkit (devrait etre present sur JP6.2)
if ! dpkg -l nvidia-container-toolkit 2>/dev/null | grep -q "^ii"; then
    log "Installation nvidia-container-toolkit..."
    sudo apt-get install -y nvidia-container-toolkit
fi

# Configuration runtime nvidia dans Docker
if [ ! -f /etc/docker/daemon.json ] || ! sudo grep -q '"nvidia"' /etc/docker/daemon.json 2>/dev/null; then
    log "Configuration runtime nvidia dans /etc/docker/daemon.json..."
    sudo nvidia-ctk runtime configure --runtime=docker
    sudo systemctl restart docker
    sleep 2
fi

# Test runtime nvidia
log "Test runtime nvidia (peut declencher un pull d'image, ~quelques minutes)..."
# --network host etait obligatoire sur JP6.2 (kernel Tegra 5.15 sans iptable_raw,
# bridge networking de Docker 29.x cassait — JETSON_ERREURS.md #3). Le kernel 6.8
# de JP7 devrait corriger, mais on conserve host (inoffensif + coherent avec la
# stack qui tourne deja en network_mode: host partout).
# Image de test = tensorrt SBSA (l4t-jetpack n'existe plus en r39/JP7).
if docker_cmd run --runtime nvidia --network host --rm "${TEST_IMAGE}" nvidia-smi >>"$LOG_FILE" 2>&1; then
    ok "Runtime nvidia fonctionnel"
else
    error "Test runtime nvidia echec — voir $LOG_FILE"
    error "Verifier: dpkg -l nvidia-container-toolkit; cat /etc/docker/daemon.json"
    fatal "GPU non accessible dans Docker — corriger avant de continuer"
fi

# =============================================================================
#  STEP 4 — Clone ou pull du repo
# =============================================================================
step "[4/8] Repo MicroscopeIBOM"

if [ -d "$REPO_DIR/.git" ]; then
    log "Repo existe a $REPO_DIR — git pull..."
    cd "$REPO_DIR"
    git fetch --all
    git pull --rebase || warn "git pull echec (continue avec l'etat actuel)"
else
    log "Clone vers $REPO_DIR..."
    mkdir -p "$(dirname "$REPO_DIR")"
    git clone "$REPO_URL" "$REPO_DIR"
    cd "$REPO_DIR"
fi
ok "Repo pret: $REPO_DIR (commit $(git rev-parse --short HEAD))"

# =============================================================================
#  STEP 5 — Regles udev RealSense
# =============================================================================
step "[5/8] udev RealSense (D405 future)"

UDEV_FILE=/etc/udev/rules.d/99-realsense-libusb.rules
if [ ! -f "$UDEV_FILE" ]; then
    log "Telechargement des regles udev RealSense..."
    if sudo curl -fsSL "https://raw.githubusercontent.com/IntelRealSense/librealsense/master/config/99-realsense-libusb.rules" \
        -o "$UDEV_FILE" 2>>"$LOG_FILE"; then
        sudo udevadm control --reload-rules
        sudo udevadm trigger
        ok "udev RealSense configure"
    else
        warn "Telechargement echec — la D405 ne sera pas reconnue tant que les regles ne sont pas en place"
    fi
else
    log "Regles udev RealSense deja en place"
fi

# =============================================================================
#  STEP 6 — Build base image
# =============================================================================
step "[6/8] Build microscope-ibom:base"

if [ "$SKIP_BUILD" = "1" ]; then
    log "SKIP (SKIP_BUILD=1)"
else
    log "Build de microscope-ibom:base — long (~90-120 min sur Jetson)"
    log "Tu peux suivre le progres: tail -f $LOG_FILE"
    cd "$REPO_DIR"
    if docker_cmd compose -f docker/compose.yml build base 2>&1 | tee -a "$LOG_FILE"; then
        ok "Image base buildee"
    else
        error "Build base echec — voir $LOG_FILE"
        error "Une fois corrige, relancer: SKIP_PERFMODE=1 bash $0"
        fatal "Build base rate"
    fi
fi

# =============================================================================
#  STEP 7 — Build dev image
# =============================================================================
step "[7/8] Build microscope-ibom:dev"

if [ "$SKIP_BUILD" = "1" ]; then
    log "SKIP (SKIP_BUILD=1)"
else
    log "Build de microscope-ibom:dev (~5-10 min)..."
    if docker_cmd compose -f docker/compose.yml build dev 2>&1 | tee -a "$LOG_FILE"; then
        ok "Image dev buildee"
    else
        error "Build dev echec — voir $LOG_FILE"
        fatal "Build dev rate"
    fi
fi

# =============================================================================
#  STEP 8 — Recap final
# =============================================================================
step "[8/8] Termine"

cat <<DONE

==============================================================================
${GRN}${BOLD}Bootstrap termine avec succes.${NC}
==============================================================================

${BOLD}Prochaines etapes :${NC}

DONE

if [ "$GROUP_ADDED" = "1" ]; then
    cat <<NEWGRP
  ${YEL}IMPORTANT :${NC} tu viens d'etre ajoute au groupe docker.
  Pour utiliser docker sans sudo, fais l'une de ces options :

    a) Reconnecter ta session SSH (ou faire 'logout' + login)
    b) Ou: newgrp docker
    c) Ou: continuer en utilisant sudo devant docker (moins propre)

NEWGRP
fi

cat <<NEXT
  ${BOLD}1. Lancer le container dev interactif :${NC}
       cd $REPO_DIR
       bash docker/run-dev.sh

  ${BOLD}2. Dans le container, compiler le projet :${NC}
       bash scripts/build_jetson.sh

  ${BOLD}3. Lancer l'app (depuis le container, avec ecran branche) :${NC}
       ./build/bin/MicroscopeIBOM

${BOLD}Documentation :${NC}
  - Plan global :        docs/JETSON_MIGRATION.md
  - Journal de session : docs/JETSON_SESSION_LOG.md
  - Erreurs connues :    docs/JETSON_ERREURS.md
  - Guide Docker :       docker/README.md

${BOLD}En cas de probleme :${NC}
  - Log de ce bootstrap : $LOG_FILE
  - Loguer toute erreur dans docs/JETSON_ERREURS.md (entree numerotee)

==============================================================================

NEXT
