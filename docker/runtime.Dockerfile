# =============================================================================
#  microscope-ibom:runtime
#  Image runtime minimale — l4t-jetpack + libs runtime apt + .so compiles
#  copies depuis microscope-ibom:base. PAS de toolchain (build-essential,
#  cmake, git, headers -dev) contrairement a l'image base (~plusieurs Go de moins).
#
#  ⚠️ Image jamais buildee a ce jour : au premier build, valider que tous les
#  noms de paquets runtime ci-dessous existent bien sur Jammy arm64 (si l'un
#  manque, le retrouver via `apt-cache search <lib>` dans le container base,
#  et logger l'ecart dans docs/JETSON_ERREURS.md).
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

ARG L4T_VERSION=r36.4.0

FROM nvcr.io/nvidia/l4t-jetpack:${L4T_VERSION}

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Europe/Paris

# -----------------------------------------------------------------------------
#  Libs runtime uniquement (pendants non--dev des paquets de base.Dockerfile)
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    locales tzdata \
    # OpenCV runtime deps
    libgtk-3-0 \
    libavcodec58 libavformat58 libswscale5 \
    libjpeg-turbo8 libpng16-16 libtiff5 \
    libtbb12 libtbbmalloc2 \
    libdc1394-25 \
    libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
    gstreamer1.0-plugins-good gstreamer1.0-libav \
    libv4l-0 v4l-utils \
    # USB (camera / RealSense)
    libusb-1.0-0 \
    libssl3 \
    # OpenGL/EGL runtime (Qt rendering)
    libgl1 libglx0 libopengl0 libegl1 libgles2 libglu1-mesa libvulkan1 \
    # Qt6 runtime (xcb platform plugin: libqt6gui6 + qt6-qpa-plugins)
    libqt6core6 libqt6gui6 libqt6widgets6 libqt6network6 \
    libqt6opengl6 libqt6openglwidgets6 libqt6concurrent6 \
    libqt6printsupport6 libqt6multimedia6 libqt6multimediawidgets6 \
    libqt6websockets6 \
    qt6-qpa-plugins \
    # Logging / PDF
    libspdlog1 libfmt8 \
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
