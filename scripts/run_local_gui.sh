#!/bin/bash
# =============================================================================
#  run_local_gui.sh — lance MicroscopeIBOM avec affichage sur l'ecran physique
#  branche au Jetson (HDMI, DisplayPort, ou USB-C ecran tactile).
#
#  Prerequis :
#    - Un ecran branche au Jetson (que tu peux voir)
#    - Tu lances ce script DEPUIS l'ecran local (pas via SSH)
#      OU via SSH mais en ayant deja une session graphique active sur le Jetson
#
#  Si tu es via SSH et que personne n'est connecte graphiquement sur le Jetson,
#  utilise plutot une session graphique directe (clavier + souris branches).
#
#  Usage :
#    bash scripts/run_local_gui.sh
#    bash scripts/run_local_gui.sh debug    # avec ASAN + logs verbeux
# =============================================================================

set -u

RED=$'\033[0;31m'
GRN=$'\033[0;32m'
YEL=$'\033[1;33m'
BLU=$'\033[1;34m'
NC=$'\033[0m'

REPO_DIR="${REPO_DIR:-$HOME/Assistant-git}"
DISPLAY="${DISPLAY:-:0}"
MODE="${1:-release}"

cd "$REPO_DIR" || { echo "${RED}[ERR] Repo introuvable: $REPO_DIR${NC}"; exit 1; }

echo "${BLU}[run-local]${NC} DISPLAY=$DISPLAY mode=$MODE"

# -----------------------------------------------------------------------------
# 1. Verifier qu'on a bien un display X actif
# -----------------------------------------------------------------------------
if ! xset -display "$DISPLAY" q > /dev/null 2>&1; then
    echo "${RED}[ERR] Pas de serveur X actif sur DISPLAY=$DISPLAY${NC}"
    echo "       - Verifier que l'ecran est branche et qu'une session graphique tourne"
    echo "       - Si tu es via SSH : connecte-toi physiquement au Jetson une fois pour demarrer le bureau"
    exit 1
fi

# -----------------------------------------------------------------------------
# 2. Autoriser Docker a afficher sur ce DISPLAY
#    xhost +local:docker reste un filet de securite, mais on peuple aussi un
#    vrai cookie xauth (plus propre, fonctionne meme si xhost echoue).
# -----------------------------------------------------------------------------
xhost +local:docker > /dev/null 2>&1 || echo "${YEL}[warn] xhost echec (peut etre OK)${NC}"

# -----------------------------------------------------------------------------
# 3. Override compose pour activer les devices (GPU display, input)
# -----------------------------------------------------------------------------
COMPOSE_FILES=(-f docker/compose.yml -f docker/compose.local.yml)

# -----------------------------------------------------------------------------
# 3b. Cameras USB : override genere dynamiquement avec les /dev/video* presents.
#     Un device mappe en dur mais absent empeche le container de demarrer
#     (erreurs #6/#15) — ici, pas de camera = pas de mapping = l'app demarre
#     quand meme (liste cameras vide). Brancher la camera puis relancer ce
#     script suffit (le container est recree avec les nouveaux devices).
# -----------------------------------------------------------------------------
CAM_OVERRIDE=/tmp/microscope-ibom.cameras.yml
HAVE_VIDEO=false
compgen -G "/dev/video*" > /dev/null && HAVE_VIDEO=true

# Intel RealSense (D405 = VID 8086) uses libusb (FORCE_RSUSB_BACKEND) — it needs
# the USB bus mapped, NOT /dev/video*. Detect it so we add /dev/bus/usb.
HAVE_REALSENSE=false
# Match the D405 specifically (Intel VID 8086, PID 0b5b) — a broad "8086:0b"
# match would catch unrelated Intel devices and needlessly map the USB bus.
if command -v lsusb >/dev/null 2>&1 && lsusb | grep -qiE "8086:0b5b"; then
    HAVE_REALSENSE=true
fi

if [ "$HAVE_VIDEO" = true ] || [ "$HAVE_REALSENSE" = true ]; then
    {
        echo "# Genere par run_local_gui.sh — ne pas editer (regenere a chaque lancement)"
        echo "services:"
        echo "  dev:"
        echo "    devices:"
        if [ "$HAVE_VIDEO" = true ]; then
            for v in /dev/video*; do
                echo "      - ${v}:${v}"
            done
        fi
        if [ "$HAVE_REALSENSE" = true ]; then
            # Whole USB bus for librealsense (RSUSB userspace backend).
            echo "      - /dev/bus/usb:/dev/bus/usb"
        fi
    } > "$CAM_OVERRIDE"
    COMPOSE_FILES+=(-f "$CAM_OVERRIDE")
    [ "$HAVE_VIDEO" = true ] && echo "${GRN}[run-local]${NC} Cameras V4L2: $(echo /dev/video* )"
    [ "$HAVE_REALSENSE" = true ] && echo "${GRN}[run-local]${NC} RealSense detectee (USB bus mappe pour librealsense)."
else
    rm -f "$CAM_OVERRIDE"
    echo "${YEL}[run-local]${NC} Aucune camera detectee — demarrage sans camera."
    echo "             Brancher la camera (USB ou RealSense) puis relancer ce script."
fi

# -----------------------------------------------------------------------------
# 4. Demarrer le container dev s'il ne tourne pas
# -----------------------------------------------------------------------------
# S'assurer que /tmp/.docker.xauth existe en fichier (pas dossier)
if [ -d /tmp/.docker.xauth ]; then
    sudo rm -rf /tmp/.docker.xauth
fi
touch /tmp/.docker.xauth 2>/dev/null || sudo touch /tmp/.docker.xauth
chmod 644 /tmp/.docker.xauth 2>/dev/null || sudo chmod 644 /tmp/.docker.xauth

# Peupler un vrai cookie xauth pour ce DISPLAY (famille FamilyWild via 'ffff'
# pour qu'il marche aussi en network_mode host). Si xauth absent, on se rabat
# sur le seul xhost ci-dessus.
if command -v xauth >/dev/null 2>&1; then
    xauth nlist "$DISPLAY" 2>/dev/null | sed -e 's/^..../ffff/' \
        | xauth -f /tmp/.docker.xauth nmerge - 2>/dev/null \
        || echo "${YEL}[warn] xauth nmerge echec (xhost prendra le relais)${NC}"
fi

# Pas de --force-recreate : 'compose up -d' recree le container seulement si la
# config (compose, devices) a change, sinon il reutilise l'existant — on ne tue
# plus un build/une session en cours dans le container dev a chaque lancement.
echo "${BLU}[run-local]${NC} Demarrage du container dev avec devices locaux..."
docker compose "${COMPOSE_FILES[@]}" up -d dev

# -----------------------------------------------------------------------------
# 5. Lancer le binaire dans le container avec le DISPLAY local
# -----------------------------------------------------------------------------
# Le build debug (ASAN) sort dans build-debug/ — pointer le bon binaire selon
# le mode, sinon ASAN_OPTIONS n'a aucun effet sur un binaire Release.
EXTRA_ENV=""
if [ "$MODE" = "debug" ] || [ "$MODE" = "Debug" ]; then
    BIN=build-debug/bin/MicroscopeIBOM
    EXTRA_ENV="-e QT_LOGGING_RULES=*.debug=true -e ASAN_OPTIONS=detect_leaks=0"
else
    BIN=build/bin/MicroscopeIBOM
fi

if [ ! -f "$BIN" ]; then
    echo "${RED}[ERR] Binaire introuvable: $BIN${NC}"
    echo "       Lancer d'abord : bash scripts/build_jetson.sh ${MODE}"
    exit 1
fi

echo "${GRN}[run-local]${NC} Lancement de ${BIN} dans le container..."
echo "${GRN}[run-local]${NC} Fenetre Qt6 devrait apparaitre sur ton ecran."
echo

# shellcheck disable=SC2086
docker compose "${COMPOSE_FILES[@]}" exec \
    -e DISPLAY="$DISPLAY" \
    -e XAUTHORITY=/tmp/.docker.xauth \
    $EXTRA_ENV \
    dev "./${BIN}"
