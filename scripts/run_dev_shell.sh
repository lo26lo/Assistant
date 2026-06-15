#!/bin/bash
# =============================================================================
#  run_dev_shell.sh — ouvre un shell DEV interactif dans le container, avec
#  les memes integrations que run_local_gui.sh : cameras USB (/dev/video*),
#  GPU display + X11. C'est le script a utiliser pour BUILDER / DEBUGGER en
#  ayant la camera accessible depuis le shell.
#
#  Difference avec les autres scripts :
#    - docker/run-dev.sh    : shell dev MAIS sans /dev/video* (pas de camera)
#    - run_local_gui.sh     : lance directement le binaire (pas de shell)
#    - run_dev_shell.sh     : shell dev AVEC camera + X11   <-- celui-ci
#
#  Usage :
#    bash scripts/run_dev_shell.sh
#
#  Une fois dans le shell, exemples :
#    bash scripts/build_jetson.sh
#    cd build && ctest --output-on-failure
#    ./build/bin/MicroscopeIBOM          # la camera est mappee
# =============================================================================

set -u

RED=$'\033[0;31m'
GRN=$'\033[0;32m'
YEL=$'\033[1;33m'
BLU=$'\033[1;34m'
NC=$'\033[0m'

REPO_DIR="${REPO_DIR:-$HOME/Assistant-git}"
DISPLAY="${DISPLAY:-:0}"

cd "$REPO_DIR" || { echo "${RED}[ERR] Repo introuvable: $REPO_DIR${NC}"; exit 1; }

echo "${BLU}[dev-shell]${NC} DISPLAY=$DISPLAY"

# -----------------------------------------------------------------------------
# 1. Autoriser Docker a afficher sur ce DISPLAY (filet de securite) + cookie
#    xauth propre. Non bloquant si pas de serveur X (on peut vouloir juste
#    builder sans GUI) — on previent mais on continue.
# -----------------------------------------------------------------------------
if xset -display "$DISPLAY" q > /dev/null 2>&1; then
    xhost +local:docker > /dev/null 2>&1 || echo "${YEL}[warn] xhost echec (peut etre OK)${NC}"

    if [ -d /tmp/.docker.xauth ]; then
        sudo rm -rf /tmp/.docker.xauth
    fi
    touch /tmp/.docker.xauth 2>/dev/null || sudo touch /tmp/.docker.xauth
    chmod 644 /tmp/.docker.xauth 2>/dev/null || sudo chmod 644 /tmp/.docker.xauth

    if command -v xauth >/dev/null 2>&1; then
        xauth nlist "$DISPLAY" 2>/dev/null | sed -e 's/^..../ffff/' \
            | xauth -f /tmp/.docker.xauth nmerge - 2>/dev/null \
            || echo "${YEL}[warn] xauth nmerge echec (xhost prendra le relais)${NC}"
    fi
else
    echo "${YEL}[dev-shell]${NC} Pas de serveur X sur DISPLAY=$DISPLAY — shell sans GUI (build/test OK)."
fi

# -----------------------------------------------------------------------------
# 2. Override compose : devices locaux (GPU display, input)
# -----------------------------------------------------------------------------
COMPOSE_FILES=(-f docker/compose.yml -f docker/compose.local.yml)

# -----------------------------------------------------------------------------
# 3. Cameras USB : override genere dynamiquement avec les /dev/video* presents.
#    (Meme logique que run_local_gui.sh — un device absent mappe en dur
#    empeche le container de demarrer, erreurs #6/#15.)
# -----------------------------------------------------------------------------
CAM_OVERRIDE=/tmp/microscope-ibom.cameras.yml
HAVE_VIDEO=false
compgen -G "/dev/video*" > /dev/null && HAVE_VIDEO=true

# Intel RealSense (D405 = VID 8086) uses libusb (FORCE_RSUSB_BACKEND) — it needs
# the USB bus mapped, NOT /dev/video*.
HAVE_REALSENSE=false
# Match the D405 specifically (Intel VID 8086, PID 0b5b) — a broad "8086:0b"
# match would catch unrelated Intel devices and needlessly map the USB bus.
if command -v lsusb >/dev/null 2>&1 && lsusb | grep -qiE "8086:0b5b"; then
    HAVE_REALSENSE=true
fi

if [ "$HAVE_VIDEO" = true ] || [ "$HAVE_REALSENSE" = true ]; then
    {
        echo "# Genere par run_dev_shell.sh — ne pas editer (regenere a chaque lancement)"
        echo "services:"
        echo "  dev:"
        echo "    devices:"
        if [ "$HAVE_VIDEO" = true ]; then
            for v in /dev/video*; do
                echo "      - ${v}:${v}"
            done
        fi
        if [ "$HAVE_REALSENSE" = true ]; then
            echo "      - /dev/bus/usb:/dev/bus/usb"
        fi
    } > "$CAM_OVERRIDE"
    COMPOSE_FILES+=(-f "$CAM_OVERRIDE")
    [ "$HAVE_VIDEO" = true ] && echo "${GRN}[dev-shell]${NC} Cameras V4L2: $(echo /dev/video* )"
    [ "$HAVE_REALSENSE" = true ] && echo "${GRN}[dev-shell]${NC} RealSense detectee (USB bus mappe)."
else
    rm -f "$CAM_OVERRIDE"
    echo "${YEL}[dev-shell]${NC} Aucune camera detectee — shell sans camera."
    echo "             Brancher la camera (USB ou RealSense) puis relancer ce script."
fi

# -----------------------------------------------------------------------------
# 4. Demarrer / reutiliser le container dev (recree seulement si la config a
#    change, ex: nouvelle camera branchee).
# -----------------------------------------------------------------------------
echo "${BLU}[dev-shell]${NC} Demarrage du container dev avec devices locaux..."
docker compose "${COMPOSE_FILES[@]}" up -d dev || {
    echo "${RED}[ERR] docker compose up a echoue${NC}"; exit 1;
}

# -----------------------------------------------------------------------------
# 5. Shell interactif dans le container, avec DISPLAY + xauth exportes pour
#    pouvoir lancer le binaire GUI a la main.
# -----------------------------------------------------------------------------
echo "${GRN}[dev-shell]${NC} Connexion interactive (Ctrl+D pour sortir)."
echo "${GRN}[dev-shell]${NC} La camera et l'ecran sont accessibles depuis ce shell."
echo

docker compose "${COMPOSE_FILES[@]}" exec \
    -e DISPLAY="$DISPLAY" \
    -e XAUTHORITY=/tmp/.docker.xauth \
    dev bash

echo
echo "${BLU}[dev-shell]${NC} Container toujours actif en arriere-plan."
echo "  Reconnecter :  bash scripts/run_dev_shell.sh"
echo "  Lancer GUI  :  bash scripts/run_local_gui.sh"
echo "  Arreter     :  docker compose -f docker/compose.yml down"
