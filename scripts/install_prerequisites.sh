#!/bin/bash
# ============================================================================
#  MicroscopeIBOM — Installation des prérequis Linux
#  Date: 2026-03-18
#  Cible: Ubuntu 22.04+ / Debian 12+ avec GPU NVIDIA RTX 5070
# ============================================================================
#
#  Usage: sudo ./install_prerequisites.sh
#
# ============================================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

TOOLS_DIR="$HOME/Tools"
VCPKG_DIR="$TOOLS_DIR/vcpkg"

echo ""
echo "============================================================================"
echo " MicroscopeIBOM — Installation des prérequis Linux"
echo "============================================================================"
echo ""

# --- Vérification root/sudo ---
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}[ERREUR] Ce script doit être exécuté avec sudo${NC}"
    echo "Usage: sudo ./install_prerequisites.sh"
    exit 1
fi

REAL_USER=${SUDO_USER:-$USER}
REAL_HOME=$(eval echo ~$REAL_USER)

# ============================================================================
#  ÉTAPE 1 : Mise à jour système
# ============================================================================
echo "[1/10] Mise à jour du système..."
apt update && apt upgrade -y
echo -e "${GREEN}  Système mis à jour.${NC}"
echo ""

# ============================================================================
#  ÉTAPE 2 : Outils de build essentiels
# ============================================================================
echo "[2/10] Installation des outils de build..."
apt install -y \
    build-essential \
    gcc-13 g++-13 \
    cmake \
    ninja-build \
    git \
    curl \
    wget \
    unzip \
    pkg-config \
    autoconf \
    automake \
    libtool

# Mettre gcc-13 par défaut
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100
update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

echo -e "${GREEN}  Outils de build installés.${NC}"
echo ""

# ============================================================================
#  ÉTAPE 3 : Dépendances système
# ============================================================================
echo "[3/10] Installation des dépendances système..."
apt install -y \
    libgl1-mesa-dev \
    libglu1-mesa-dev \
    libx11-dev \
    libxkbcommon-dev \
    libxkbcommon-x11-dev \
    libwayland-dev \
    libvulkan-dev \
    libasound2-dev \
    libpulse-dev \
    libv4l-dev \
    v4l-utils \
    libusb-1.0-0-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-good \
    libssl-dev \
    libfontconfig1-dev \
    libfreetype6-dev \
    libdbus-1-dev \
    libegl1-mesa-dev

echo -e "${GREEN}  Dépendances système installées.${NC}"
echo ""

# ============================================================================
#  ÉTAPE 4 : Python 3.11+
# ============================================================================
echo "[4/10] Installation de Python 3.11..."
apt install -y python3.11 python3.11-venv python3-pip python3.11-dev
update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.11 100

echo -e "${GREEN}  Python 3.11 installé.${NC}"
echo ""

# ============================================================================
#  ÉTAPE 5 : NVIDIA Drivers + CUDA
# ============================================================================
echo "[5/10] Installation NVIDIA Drivers + CUDA Toolkit..."

# Ajouter le repo NVIDIA
if ! dpkg -l | grep -q "cuda-toolkit-12"; then
    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
    dpkg -i cuda-keyring_1.1-1_all.deb
    rm cuda-keyring_1.1-1_all.deb
    apt update
    apt install -y cuda-toolkit-12-6
    echo -e "${GREEN}  CUDA Toolkit 12.6 installé.${NC}"
else
    echo -e "${YELLOW}  CUDA Toolkit déjà installé. Skip.${NC}"
fi

# Variables d'environnement CUDA
CUDA_PROFILE="/etc/profile.d/cuda.sh"
if [ ! -f "$CUDA_PROFILE" ]; then
    cat > "$CUDA_PROFILE" << 'EOF'
export CUDA_HOME=/usr/local/cuda
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=$CUDA_HOME/lib64:$LD_LIBRARY_PATH
EOF
    source "$CUDA_PROFILE"
fi

echo ""

# ============================================================================
#  ÉTAPE 6 : vcpkg
# ============================================================================
echo "[6/10] Installation de vcpkg..."
if [ -f "$VCPKG_DIR/vcpkg" ]; then
    echo -e "${YELLOW}  vcpkg déjà installé. Skip.${NC}"
else
    sudo -u $REAL_USER mkdir -p "$TOOLS_DIR"
    cd "$TOOLS_DIR"
    sudo -u $REAL_USER git clone https://github.com/microsoft/vcpkg.git
    cd vcpkg
    sudo -u $REAL_USER ./bootstrap-vcpkg.sh -disableMetrics

    # Ajouter au PATH
    BASHRC="$REAL_HOME/.bashrc"
    if ! grep -q "VCPKG_ROOT" "$BASHRC"; then
        echo "" >> "$BASHRC"
        echo "# vcpkg" >> "$BASHRC"
        echo "export VCPKG_ROOT=$VCPKG_DIR" >> "$BASHRC"
        echo 'export PATH=$VCPKG_ROOT:$PATH' >> "$BASHRC"
    fi
    echo -e "${GREEN}  vcpkg installé.${NC}"
fi

export VCPKG_ROOT="$VCPKG_DIR"
export PATH="$VCPKG_ROOT:$PATH"
echo ""

# ============================================================================
#  ÉTAPE 7 : Dépendances vcpkg
# ============================================================================
echo "[7/10] Installation des dépendances vcpkg (peut prendre 30-60 min)..."
TRIPLET="x64-linux"

sudo -u $REAL_USER "$VCPKG_DIR/vcpkg" install opencv4[core,dnn,highgui,imgproc,videoio]:$TRIPLET
sudo -u $REAL_USER "$VCPKG_DIR/vcpkg" install nlohmann-json:$TRIPLET
sudo -u $REAL_USER "$VCPKG_DIR/vcpkg" install spdlog:$TRIPLET
sudo -u $REAL_USER "$VCPKG_DIR/vcpkg" install catch2:$TRIPLET
sudo -u $REAL_USER "$VCPKG_DIR/vcpkg" install onnxruntime-gpu:$TRIPLET
sudo -u $REAL_USER "$VCPKG_DIR/vcpkg" install cpr:$TRIPLET
sudo -u $REAL_USER "$VCPKG_DIR/vcpkg" install zxing-cpp:$TRIPLET
sudo -u $REAL_USER "$VCPKG_DIR/vcpkg" install libharu:$TRIPLET

echo -e "${GREEN}  Dépendances vcpkg installées.${NC}"
echo ""

# ============================================================================
#  ÉTAPE 8 : Qt6
# ============================================================================
echo "[8/10] Installation de Qt6..."
apt install -y \
    qt6-base-dev \
    qt6-multimedia-dev \
    qt6-declarative-dev \
    qt6-tools-dev \
    qt6-l10n-tools \
    qt6-shader-baker \
    libqt6opengl6-dev \
    libqt6websockets6-dev \
    qml6-module-qtmultimedia \
    qml6-module-qtquick

echo -e "${GREEN}  Qt6 installé.${NC}"
echo ""

# ============================================================================
#  ÉTAPE 9 : Paquets Python (training IA)
# ============================================================================
echo "[9/10] Installation des paquets Python (training IA)..."
sudo -u $REAL_USER pip install --user --upgrade pip
sudo -u $REAL_USER pip install --user ultralytics
sudo -u $REAL_USER pip install --user onnx onnxruntime-gpu
sudo -u $REAL_USER pip install --user torch torchvision --index-url https://download.pytorch.org/whl/cu124
sudo -u $REAL_USER pip install --user opencv-python
sudo -u $REAL_USER pip install --user beautifulsoup4
sudo -u $REAL_USER pip install --user labelimg
sudo -u $REAL_USER pip install --user tensorrt

echo -e "${GREEN}  Paquets Python installés.${NC}"
echo ""

# ============================================================================
#  ÉTAPE 10 : Instructions manuelles
# ============================================================================
echo "[10/10] Éléments nécessitant une installation MANUELLE :"
echo ""
echo "============================================================================"
echo " ACTIONS MANUELLES REQUISES :"
echo "============================================================================"
echo ""
echo " 1. cuDNN 9.x :"
echo "    - Créer un compte NVIDIA Developer (gratuit)"
echo "    - Télécharger depuis: https://developer.nvidia.com/cudnn"
echo "    - sudo dpkg -i cudnn-local-repo-*.deb"
echo "    - sudo apt install libcudnn9-cuda-12 libcudnn9-dev-cuda-12"
echo ""
echo " 2. TensorRT 10.x :"
echo "    - Télécharger depuis: https://developer.nvidia.com/tensorrt"
echo "    - sudo dpkg -i nv-tensorrt-repo-*.deb"
echo "    - sudo apt install tensorrt"
echo "    - Ajouter /usr/lib/x86_64-linux-gnu/ au LD_LIBRARY_PATH si nécessaire"
echo ""
echo " 3. NVIDIA Drivers (si pas déjà installés) :"
echo "    - sudo apt install nvidia-driver-560"
echo "    - Redémarrer le PC"
echo ""
echo "============================================================================"
echo ""

# ============================================================================
#  Résumé
# ============================================================================
echo "============================================================================"
echo " RÉSUMÉ DE L'INSTALLATION"
echo "============================================================================"
echo ""

check_cmd() {
    if command -v "$1" &> /dev/null; then
        echo -e "  ${GREEN}[OK]${NC} $2"
    else
        echo -e "  ${RED}[!!]${NC} $2 NON TROUVÉ"
    fi
}

check_cmd git "Git"
check_cmd cmake "CMake"
check_cmd ninja "Ninja"
check_cmd python3 "Python 3"
check_cmd g++ "G++ (compilateur C++)"
check_cmd nvcc "CUDA Toolkit"
[ -f "$VCPKG_DIR/vcpkg" ] && echo -e "  ${GREEN}[OK]${NC} vcpkg" || echo -e "  ${RED}[!!]${NC} vcpkg NON TROUVÉ"

echo ""
echo " Éléments manuels à vérifier :"
echo "   [ ] cuDNN installé"
echo "   [ ] TensorRT installé"
echo "   [ ] Drivers NVIDIA à jour"
echo ""
echo "============================================================================"
echo " Installation terminée. Redémarrez le terminal (ou source ~/.bashrc)"
echo " puis procédez aux installations manuelles."
echo "============================================================================"
echo ""
