# =============================================================================
#  microscope-ibom:runtime
#  Image runtime minimale — base + binaire compile + entrypoint
# =============================================================================
#
#  Prerequis: avoir compile le binaire dans le container dev:
#    docker compose -f docker/compose.yml exec dev bash scripts/build_jetson.sh
#
#  Build:
#    docker compose -f docker/compose.yml build runtime
#
#  Run:
#    docker compose -f docker/compose.yml up runtime
#
# =============================================================================

FROM microscope-ibom:base

# -----------------------------------------------------------------------------
#  Binaire + ressources statiques (les models/configs sont volumes au runtime)
# -----------------------------------------------------------------------------
COPY build/bin/MicroscopeIBOM /opt/microscope-ibom/MicroscopeIBOM
COPY resources/               /opt/microscope-ibom/resources/

# Stripper les symboles pour reduire la taille
RUN strip /opt/microscope-ibom/MicroscopeIBOM || true

# -----------------------------------------------------------------------------
#  Entrypoint
# -----------------------------------------------------------------------------
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# -----------------------------------------------------------------------------
#  Volumes attendus (mountes par compose.yml ou docker run)
# -----------------------------------------------------------------------------
VOLUME ["/opt/microscope-ibom/config"]
VOLUME ["/opt/microscope-ibom/logs"]
VOLUME ["/opt/microscope-ibom/models"]
VOLUME ["/opt/microscope-ibom/tensorrt-cache"]

ENTRYPOINT ["/entrypoint.sh"]
