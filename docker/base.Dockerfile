# =============================================================================
#  microscope-ibom:base
#  Image de base avec Qt6 + OpenCV CUDA + TensorRT (de JetPack) + librealsense
#  Cible: Jetson AGX Orin 32GB sur JetPack 6.2 (L4T R36.4)
# =============================================================================
#
#  Build:
#    docker compose -f docker/compose.yml build base
#    OU
#    docker build -f docker/base.Dockerfile -t microscope-ibom:base .
#
#  Premier build: ~90-120 min (compilation OpenCV CUDA from source)
#  Builds suivants: <5 min (cache Docker)
#
# =============================================================================

ARG L4T_VERSION=r36.4.0
ARG OPENCV_VERSION=4.10.0
ARG REALSENSE_VERSION=v2.55.1
ARG ONNXRUNTIME_VERSION=v1.19.2
ARG CUDA_ARCH_BIN=8.7

# =============================================================================
#  STAGE 1 : OpenCV with CUDA (build from source)
# =============================================================================
FROM nvcr.io/nvidia/l4t-jetpack:${L4T_VERSION} AS opencv-builder

ARG OPENCV_VERSION
ARG CUDA_ARCH_BIN

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git pkg-config wget unzip \
    libgtk-3-dev \
    libavcodec-dev libavformat-dev libswscale-dev \
    libv4l-dev libxvidcore-dev libx264-dev \
    libjpeg-dev libpng-dev libtiff-dev \
    gfortran openexr \
    libatlas-base-dev \
    python3-dev python3-numpy python3-pip \
    libtbb-dev libtbbmalloc2 \
    libdc1394-dev \
    libeigen3-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
RUN git clone --depth 1 --branch ${OPENCV_VERSION} https://github.com/opencv/opencv.git && \
    git clone --depth 1 --branch ${OPENCV_VERSION} https://github.com/opencv/opencv_contrib.git

WORKDIR /tmp/opencv/build
RUN cmake -G Ninja \
    -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_INSTALL_PREFIX=/usr/local \
    -D OPENCV_EXTRA_MODULES_PATH=/tmp/opencv_contrib/modules \
    -D WITH_CUDA=ON \
    -D WITH_CUDNN=ON \
    -D OPENCV_DNN_CUDA=ON \
    -D ENABLE_FAST_MATH=ON \
    -D CUDA_FAST_MATH=ON \
    -D WITH_CUBLAS=ON \
    -D WITH_NVCUVID=ON \
    -D CUDA_ARCH_BIN=${CUDA_ARCH_BIN} \
    -D CUDA_ARCH_PTX="" \
    -D WITH_GSTREAMER=ON \
    -D WITH_V4L=ON \
    -D WITH_LIBV4L=ON \
    -D WITH_OPENGL=ON \
    -D WITH_TBB=ON \
    -D WITH_EIGEN=ON \
    -D BUILD_opencv_python2=OFF \
    -D BUILD_opencv_python3=ON \
    -D OPENCV_GENERATE_PKGCONFIG=ON \
    -D BUILD_TESTS=OFF \
    -D BUILD_PERF_TESTS=OFF \
    -D BUILD_EXAMPLES=OFF \
    -D BUILD_DOCS=OFF \
    -D OPENCV_ENABLE_NONFREE=ON \
    .. && \
    ninja -j$(nproc) && \
    DESTDIR=/opencv-install ninja install


# =============================================================================
#  STAGE 2 : librealsense2 (build from source — paquet Intel non dispo ARM64)
# =============================================================================
FROM nvcr.io/nvidia/l4t-jetpack:${L4T_VERSION} AS realsense-builder

ARG REALSENSE_VERSION

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git pkg-config \
    libssl-dev libusb-1.0-0-dev libudev-dev \
    libglfw3-dev libgl1-mesa-dev libglu1-mesa-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
RUN git clone --depth 1 --branch ${REALSENSE_VERSION} https://github.com/IntelRealSense/librealsense.git

WORKDIR /tmp/librealsense/build
RUN cmake -G Ninja \
    -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_INSTALL_PREFIX=/usr/local \
    -D BUILD_EXAMPLES=OFF \
    -D BUILD_GRAPHICAL_EXAMPLES=OFF \
    -D BUILD_PYTHON_BINDINGS=OFF \
    -D BUILD_WITH_CUDA=ON \
    -D FORCE_RSUSB_BACKEND=ON \
    .. && \
    ninja -j$(nproc) && \
    DESTDIR=/realsense-install ninja install


# =============================================================================
#  STAGE 3 : ONNX Runtime avec CUDA + TensorRT EP (build from source ARM64)
#  Pas de paquet apt libonnxruntime-dev sur Jammy ARM64, pas de wheel CMake
#  installable depuis pip. Build from source = seule voie propre pour avoir
#  un onnxruntimeConfig.cmake compatible avec find_package(onnxruntime CONFIG).
#  Long (~1-2h sur Jetson AGX Orin 32GB).
# =============================================================================
FROM nvcr.io/nvidia/l4t-jetpack:${L4T_VERSION} AS onnxruntime-builder

ARG ONNXRUNTIME_VERSION

ENV DEBIAN_FRONTEND=noninteractive
ENV CUDACXX=/usr/local/cuda/bin/nvcc

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git pkg-config wget ca-certificates \
    python3 python3-dev python3-pip python3-numpy python3-setuptools python3-wheel python3-packaging \
    libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
# Clone en 2 etapes pour eviter "HTTP/2 stream was not closed cleanly: CANCEL"
# observe quand on tire ~3-4 GB en un coup via git clone --recursive sur ARM64 :
#   1. postBuffer 512MB pour les gros packs
#   2. low-speed timeouts desactives (GitHub peut throttle pendant 30s+ sur un sous-fichier)
#   3. clone principal seul (rapide, ~30 MB pour ORT v1.19.2 --depth 1)
#   4. submodules en parallele (--jobs 4) et shallow (--depth 1) avec 3 tentatives
RUN git config --global http.postBuffer 524288000 \
 && git config --global http.lowSpeedLimit 0 \
 && git config --global http.lowSpeedTime 999999 \
 && git clone --depth 1 --branch ${ONNXRUNTIME_VERSION} \
        https://github.com/microsoft/onnxruntime.git \
 && cd onnxruntime \
 && for i in 1 2 3; do \
        git submodule update --init --recursive --depth 1 --jobs 4 && break ; \
        echo "[onnx-clone] submodules attempt $i/3 failed, retry in 10s..." ; \
        sleep 10 ; \
    done \
 && git submodule status

WORKDIR /tmp/onnxruntime
RUN ./build.sh \
        --config Release \
        --update --build --parallel \
        --build_shared_lib \
        --skip_tests \
        --skip_submodule_sync \
        --allow_running_as_root \
        --use_cuda --cuda_home /usr/local/cuda \
        --cudnn_home /usr/lib/aarch64-linux-gnu \
        --use_tensorrt --tensorrt_home /usr \
        --cuda_version 12.6 \
        --cmake_extra_defines CMAKE_CUDA_ARCHITECTURES=87 \
        --cmake_extra_defines onnxruntime_BUILD_UNIT_TESTS=OFF \
    && cd build/Linux/Release \
    && DESTDIR=/onnxruntime-install cmake --install . --prefix /usr/local


# =============================================================================
#  STAGE 4 : Image finale
# =============================================================================
FROM nvcr.io/nvidia/l4t-jetpack:${L4T_VERSION}

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Europe/Paris

# -----------------------------------------------------------------------------
#  Outils système + dépendances runtime
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    build-essential ninja-build cmake git \
    pkg-config \
    curl wget unzip \
    libgtk-3-0 \
    libavcodec58 libavformat58 libswscale5 \
    libjpeg-turbo8 libpng16-16 libtiff5 \
    libtbb12 libtbbmalloc2 \
    libdc1394-25 \
    libeigen3-dev \
    libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
    gstreamer1.0-plugins-good gstreamer1.0-libav \
    libv4l-0 v4l-utils \
    libusb-1.0-0 \
    libssl3 \
    udev \
    locales \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8 fr_FR.UTF-8

# -----------------------------------------------------------------------------
#  Qt6 (Ubuntu 22.04 — Qt 6.2 LTS)
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    qt6-base-dev \
    qt6-multimedia-dev \
    qt6-tools-dev \
    qt6-tools-dev-tools \
    qt6-l10n-tools \
    qt6-shader-baker \
    qt6-declarative-dev \
    qt6-virtualkeyboard-plugin \
    qml6-module-qtquick-virtualkeyboard \
    libqt6opengl6-dev \
    libqt6openglwidgets6 \
    libqt6websockets6-dev \
    libqt6concurrent6 \
    libqt6printsupport6 \
    libqt6multimediawidgets6 \
    qml6-module-qtmultimedia \
    qml6-module-qtquick \
    qml6-module-qtquick-controls \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
#  Dépendances applicatives (alternative à vcpkg pour les paquets dispo en apt)
#  ONNX Runtime est copie depuis le stage onnxruntime-builder (pas de paquet apt
#  ARM64 dispo) — voir COPY plus bas.
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    libspdlog-dev libfmt-dev \
    nlohmann-json3-dev \
    libhpdf-dev \
    catch2 \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
#  ZXing-cpp — pas dans Ubuntu 22.04, build from source (rapide ~3 min)
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake build-essential git \
    && cd /tmp \
    && git clone --depth 1 --branch v2.2.1 https://github.com/zxing-cpp/zxing-cpp.git \
    && cd zxing-cpp && cmake -B build -G Ninja \
       -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_SHARED_LIBS=ON \
       -DBUILD_EXAMPLES=OFF \
       -DBUILD_BLACKBOX_TESTS=OFF \
    && cmake --build build -j$(nproc) \
    && cmake --install build \
    && cd / && rm -rf /tmp/zxing-cpp \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
#  Copie des artefacts compilés des stages précédents
# -----------------------------------------------------------------------------
COPY --from=opencv-builder       /opencv-install/usr/local/       /usr/local/
COPY --from=realsense-builder    /realsense-install/usr/local/    /usr/local/
COPY --from=onnxruntime-builder  /onnxruntime-install/usr/local/  /usr/local/

# -----------------------------------------------------------------------------
#  Règles udev RealSense (au cas où montées en read-only depuis l'hôte)
# -----------------------------------------------------------------------------
COPY --from=realsense-builder /tmp/librealsense/config/99-realsense-libusb.rules \
    /etc/udev/rules.d/99-realsense-libusb.rules

RUN ldconfig

# -----------------------------------------------------------------------------
#  Variables d'environnement runtime
# -----------------------------------------------------------------------------
ENV LANG=fr_FR.UTF-8
ENV LC_ALL=fr_FR.UTF-8
ENV QT_QPA_PLATFORM=xcb
ENV QT_X11_NO_MITSHM=1
ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/local/cuda/lib64:${LD_LIBRARY_PATH}
ENV CMAKE_PREFIX_PATH=/usr/local:${CMAKE_PREFIX_PATH}

WORKDIR /opt/microscope-ibom

# -----------------------------------------------------------------------------
#  Vérifications de santé (fail fast au build)
# -----------------------------------------------------------------------------
RUN echo "=== Verification stack ===" && \
    /usr/local/cuda/bin/nvcc --version | head -4 && \
    pkg-config --modversion opencv4 && \
    pkg-config --modversion realsense2 && \
    qmake6 -query QT_VERSION && \
    ls /usr/local/include/onnxruntime/ 2>/dev/null | head -3 && \
    ls /usr/local/lib/libonnxruntime* 2>/dev/null | head -3 && \
    echo "=== Stack OK ==="
