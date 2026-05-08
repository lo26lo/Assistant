#!/bin/bash
# =============================================================================
#  Entrypoint runtime — verifie l'env et lance MicroscopeIBOM
# =============================================================================

set -euo pipefail

APP_DIR=/opt/microscope-ibom
APP_BIN=${APP_DIR}/MicroscopeIBOM
TRT_CACHE=${APP_DIR}/tensorrt-cache
MODELS_DIR=${APP_DIR}/models
LOGS_DIR=${APP_DIR}/logs

# -----------------------------------------------------------------------------
#  Creation des dossiers de persistance s'ils n'existent pas
# -----------------------------------------------------------------------------
mkdir -p "${LOGS_DIR}" "${TRT_CACHE}"

# -----------------------------------------------------------------------------
#  Sanity check : binaire present
# -----------------------------------------------------------------------------
if [ ! -x "${APP_BIN}" ]; then
    echo "[entrypoint] ERREUR: ${APP_BIN} non trouve ou non executable"
    echo "[entrypoint] Avez-vous compile le binaire dans le container dev avant de builder runtime ?"
    exit 1
fi

# -----------------------------------------------------------------------------
#  Sanity check : modeles presents
# -----------------------------------------------------------------------------
if [ ! -d "${MODELS_DIR}" ] || [ -z "$(ls -A ${MODELS_DIR} 2>/dev/null)" ]; then
    echo "[entrypoint] ATTENTION: ${MODELS_DIR} vide ou absent"
    echo "[entrypoint] Monter le volume avec les .onnx via compose.yml"
fi

# -----------------------------------------------------------------------------
#  Sanity check : DISPLAY (GUI)
# -----------------------------------------------------------------------------
if [ -z "${DISPLAY:-}" ]; then
    echo "[entrypoint] ATTENTION: DISPLAY non defini, GUI ne demarrera pas"
fi

# -----------------------------------------------------------------------------
#  Sanity check : GPU
# -----------------------------------------------------------------------------
if ! nvidia-smi > /dev/null 2>&1; then
    echo "[entrypoint] ATTENTION: nvidia-smi echec — runtime nvidia manquant ?"
fi

# -----------------------------------------------------------------------------
#  Generation initiale des engines TensorRT (si absents)
# -----------------------------------------------------------------------------
if [ -d "${MODELS_DIR}" ] && [ ! -f "${TRT_CACHE}/.engines_built" ]; then
    echo "[entrypoint] Premier lancement detecte — generation des engines TensorRT"
    echo "[entrypoint] (peut prendre 5-15 min selon les modeles)"
    # Le binaire devra gerer l'option --build-engines OU
    # generer les engines au premier appel d'inference et les cacher dans TRT_CACHE
    # touch "${TRT_CACHE}/.engines_built"  # decommenter quand --build-engines existe
fi

# -----------------------------------------------------------------------------
#  Lancement
# -----------------------------------------------------------------------------
cd "${APP_DIR}"
echo "[entrypoint] Lancement de MicroscopeIBOM..."
exec "${APP_BIN}" "$@"
