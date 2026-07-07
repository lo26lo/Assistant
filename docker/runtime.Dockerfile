# =============================================================================
#  microscope-ibom:runtime
#  Image runtime — base tensorrt SBSA + libs runtime apt + .so compiles
#  copies depuis microscope-ibom:base. PAS de toolchain (build-essential,
#  cmake, git, headers -dev) contrairement a l'image base.
#
#  ⚠️ Image jamais buildee a ce jour : au premier build, valider que tous les
#  noms de paquets runtime ci-dessous existent bien sur Ubuntu 24.04 (noble)
#  arm64 — transition t64 + sonames bumpes (si l'un manque, le retrouver via
#  `apt-cache search <lib>` dans le container base, et logger l'ecart dans
#  docs/JETSON_ERREURS.md).
#
#  Prerequis: avoir compile le binaire dans le container dev:
#    docker compose -f docker/compose.yml exec dev bash scripts/build_jetson.sh
#
#  Build:
#    docker compose -f docker/compose.yml build runtime
#
#  Run:
#    docker compose -f docker/compose.yml up runtime
# =============================================================================

# Meme base que base.Dockerfile (l4t-jetpack retire de NGC en r39). Fournit
# CUDA 13.2 + TensorRT 10.16 runtime. Override possible via --build-arg.
ARG BASE_IMAGE=nvcr.io/nvidia/tensorrt:26.05-py3

FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Europe/Paris

# -----------------------------------------------------------------------------
#  Libs runtime uniquement (pendants non--dev des paquets de base.Dockerfile)
# -----------------------------------------------------------------------------
# ## CHECK Ubuntu 24.04 (noble) : transition t64 + sonames bumpes. Points
# incertains a valider en priorite : libspdlog (1.12 sur noble ? -> libspdlog1.15
# n'existe pas ; verifier `apt-cache search libspdlog`), libfmt8 -> libfmt9.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    locales tzdata \
    # OpenCV runtime deps
    libgtk-3-0t64 \
    libavcodec60 libavformat60 libswscale7 \
    libjpeg-turbo8 libpng16-16t64 libtiff6 \
    libtbb12 libtbbmalloc2 \
    libdc1394-25 \
    libgstreamer1.0-0t64 libgstreamer-plugins-base1.0-0t64 \
    gstreamer1.0-plugins-good gstreamer1.0-libav \
    libv4l-0t64 v4l-utils \
    # USB (camera / RealSense)
    libusb-1.0-0t64 \
    libssl3t64 \
    # OpenGL/EGL runtime (Qt rendering) — libglvnd, pas de t64
    libgl1 libglx0 libopengl0 libegl1 libgles2 libglu1-mesa libvulkan1 \
    # Qt6 runtime (xcb platform plugin: libqt6gui6t64 + qt6-qpa-plugins)
    libqt6core6t64 libqt6gui6t64 libqt6widgets6t64 libqt6network6t64 \
    libqt6opengl6t64 libqt6openglwidgets6t64 libqt6concurrent6t64 \
    libqt6printsupport6t64 libqt6multimedia6t64 libqt6multimediawidgets6t64 \
    libqt6websockets6t64 \
    qt6-qpa-plugins \
    # Logging / PDF (## CHECK sonames noble : libspdlog / libfmt9)
    libspdlog1.12 libfmt9 \
    libhpdf-2.3.0 \
    && rm -rf /var/lib/apt/lists/*

RUN locale-gen fr_FR.UTF-8

# -----------------------------------------------------------------------------
#  Artefacts compiles from-source de l'image base : OpenCV CUDA, ONNX Runtime,
#  librealsense2, ZXing-cpp. On ne copie que les libs partagees (pas les
#  headers ni les outils) — c'est tout ce que le binaire dlopen/link.
# -----------------------------------------------------------------------------
COPY --from=microscope-ibom:base /usr/local/lib/ /usr/local/lib/
RUN ldconfig

# -----------------------------------------------------------------------------
#  Binaire + ressources statiques (models/config sont des volumes au runtime)
# -----------------------------------------------------------------------------
COPY build/bin/MicroscopeIBOM /opt/microscope-ibom/MicroscopeIBOM
COPY resources/               /opt/microscope-ibom/resources/

# Stripper les symboles pour reduire la taille
RUN strip /opt/microscope-ibom/MicroscopeIBOM || true

# Sanity check au build : toutes les libs du binaire doivent resoudre.
# Echoue le build tout de suite plutot qu'un crash au demarrage du container.
RUN if ldd /opt/microscope-ibom/MicroscopeIBOM | grep "not found"; then \
        echo "ERREUR: dependances manquantes (voir liste ci-dessus)"; exit 1; \
    fi

# -----------------------------------------------------------------------------
#  Entrypoint
# -----------------------------------------------------------------------------
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENV LANG=fr_FR.UTF-8
ENV LC_ALL=fr_FR.UTF-8
ENV QT_QPA_PLATFORM=xcb
ENV QT_X11_NO_MITSHM=1
ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/local/cuda/lib64:${LD_LIBRARY_PATH}

# -----------------------------------------------------------------------------
#  Volumes attendus (mountes par compose.yml ou docker run)
#  data/ regroupe config.json + calibration.yml + snapshots + cache TRT
#  (IBOM_DATA_DIR, cf src/utils/Paths.h).
# -----------------------------------------------------------------------------
VOLUME ["/opt/microscope-ibom/data"]
VOLUME ["/opt/microscope-ibom/logs"]
VOLUME ["/opt/microscope-ibom/models"]

ENTRYPOINT ["/entrypoint.sh"]
