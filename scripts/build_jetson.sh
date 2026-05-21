#!/bin/bash
# =============================================================================
#  Build MicroscopeIBOM dans le container dev
#  Usage (depuis l'interieur du container microscope-ibom-dev):
#    bash scripts/build_jetson.sh           # Release
#    bash scripts/build_jetson.sh debug     # Debug avec ASAN
# =============================================================================

set -euo pipefail

BUILD_TYPE=${1:-Release}
BUILD_DIR=build

if [ "${BUILD_TYPE}" = "debug" ] || [ "${BUILD_TYPE}" = "Debug" ]; then
    BUILD_TYPE=Debug
    BUILD_DIR=build-debug
    EXTRA_FLAGS="-DIBOM_ENABLE_ASAN=ON"
else
    BUILD_TYPE=Release
    EXTRA_FLAGS=""
fi

echo "============================================================"
echo " MicroscopeIBOM — Build ${BUILD_TYPE} (Jetson AGX Orin)"
echo "============================================================"

# -----------------------------------------------------------------------------
#  Verification environnement
# -----------------------------------------------------------------------------
if [ -z "${IBOM_PLATFORM_LINUX:-}" ] && [ ! -f /etc/nv_tegra_release ]; then
    echo "[ATTENTION] /etc/nv_tegra_release absent — pas dans un container Jetson ?"
fi

if ! command -v cmake &> /dev/null; then
    echo "[ERREUR] cmake non installe"
    exit 1
fi

# -----------------------------------------------------------------------------
#  Configuration CMake
# -----------------------------------------------------------------------------
NPROC=$(nproc)
# Limiter -j sur Jetson 32GB pour eviter OOM lors de la compilation Qt MOC
JOBS=$(( NPROC > 6 ? 6 : NPROC ))

echo "[build] Configuration (-j${JOBS})..."

# Ajouter le path multiarch Debian/Ubuntu au CMAKE_PREFIX_PATH pour que
# find_package(Qt6), find_package(OpenCV), etc. trouvent les paquets apt
# systeme. Sur Jammy arm64 : /usr/lib/aarch64-linux-gnu/cmake/Qt6/
# (et idem pour x86_64 : /usr/lib/x86_64-linux-gnu/cmake/).
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo aarch64-linux-gnu)
export CMAKE_PREFIX_PATH="/usr/lib/${MULTIARCH}/cmake:${CMAKE_PREFIX_PATH:-}"
echo "[build] CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"

CMAKE_ARGS=(
    -B "${BUILD_DIR}"
    -G Ninja
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DIBOM_ENABLE_TENSORRT=ON
    -DIBOM_ENABLE_VOICE=OFF
    -DIBOM_ENABLE_REMOTE=ON
    -DIBOM_ENABLE_TESTS=ON
    -DIBOM_ENABLE_UMA=ON
)

# vcpkg uniquement si VCPKG_ROOT defini ET fichier toolchain present
if [ -n "${VCPKG_ROOT:-}" ] && [ -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "[build] vcpkg detecte: ${VCPKG_ROOT}"
    CMAKE_ARGS+=(
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        -DVCPKG_TARGET_TRIPLET=arm64-linux
    )
else
    echo "[build] Pas de vcpkg — utilisation des paquets apt systeme"
fi

if [ -n "${EXTRA_FLAGS}" ]; then
    CMAKE_ARGS+=(${EXTRA_FLAGS})
fi

cmake "${CMAKE_ARGS[@]}"

# -----------------------------------------------------------------------------
#  Compilation
# -----------------------------------------------------------------------------
echo "[build] Compilation..."
cmake --build "${BUILD_DIR}" -j${JOBS}

# -----------------------------------------------------------------------------
#  Affichage du resultat
# -----------------------------------------------------------------------------
if [ -f "${BUILD_DIR}/bin/MicroscopeIBOM" ]; then
    echo ""
    echo "============================================================"
    echo " Build OK — binaire: ${BUILD_DIR}/bin/MicroscopeIBOM"
    ls -lh "${BUILD_DIR}/bin/MicroscopeIBOM"
    echo "============================================================"
else
    echo "[ERREUR] Binaire non genere"
    exit 1
fi

# -----------------------------------------------------------------------------
#  Tests (optionnel — commenter si trop long)
# -----------------------------------------------------------------------------
if [ "${BUILD_TYPE}" = "Debug" ] || [ "${RUN_TESTS:-0}" = "1" ]; then
    echo "[build] Lancement des tests..."
    cd "${BUILD_DIR}" && ctest --output-on-failure
fi
