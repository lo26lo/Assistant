#!/bin/bash
# =============================================================================
#  Wrapper pour lancer le container dev avec X11 + GPU
#  Usage: ./docker/run-dev.sh
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# -----------------------------------------------------------------------------
#  Verifications prealables
# -----------------------------------------------------------------------------
if ! command -v docker &> /dev/null; then
    echo "[ERREUR] Docker non installe. sudo apt install docker.io docker-compose-v2"
    exit 1
fi

if ! docker info 2>/dev/null | grep -q "Runtimes:.*nvidia"; then
    echo "[ATTENTION] Le runtime nvidia ne semble pas configure dans Docker."
    echo "  Verifier: cat /etc/docker/daemon.json"
    echo "  Doit contenir: \"runtimes\": { \"nvidia\": { ... } }"
    echo "  Si manquant: sudo apt install nvidia-container-toolkit && sudo systemctl restart docker"
fi

# -----------------------------------------------------------------------------
#  Setup X11 (autoriser le container a afficher sur l'ecran hote)
# -----------------------------------------------------------------------------
if [ -z "${DISPLAY:-}" ]; then
    export DISPLAY=:0
fi

# Fichier xauth partage avec le container
XAUTH=/tmp/.docker.xauth
touch "${XAUTH}"
xauth nlist "${DISPLAY}" 2>/dev/null | sed -e 's/^..../ffff/' | xauth -f "${XAUTH}" nmerge - || true
chmod 644 "${XAUTH}"

# Autoriser l'acces X11 local depuis Docker
xhost +local:docker > /dev/null 2>&1 || echo "[ATTENTION] xhost echec — GUI peut ne pas fonctionner"

# -----------------------------------------------------------------------------
#  Lancement
# -----------------------------------------------------------------------------
cd "${REPO_ROOT}"

echo "[run-dev] Lancement du container microscope-ibom-dev..."
docker compose -f docker/compose.yml up -d dev

echo "[run-dev] Connexion interactive (Ctrl+D pour sortir)..."
docker compose -f docker/compose.yml exec dev bash

# -----------------------------------------------------------------------------
#  Cleanup au exit (optionnel — le container reste up en arriere-plan)
# -----------------------------------------------------------------------------
echo ""
echo "[run-dev] Container toujours actif en arriere-plan."
echo "  Pour reconnecter:  docker compose -f docker/compose.yml exec dev bash"
echo "  Pour arreter:      docker compose -f docker/compose.yml down"
