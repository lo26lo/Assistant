#!/usr/bin/env bash
# Build & run the CI-buildable unit tests with plain g++ + apt packages — no
# ONNX Runtime, no CUDA, no project CMake (the root CMakeLists requires the
# full Jetson stack: onnxruntime CONFIG, ZXing, Qt6 Widgets…).
#
# Trick (validated in-session 2026-07-06, docs/INVESTIGATION_360_2026-07.md
# §8.1): the test binaries only consume ai::Detection from InferenceEngine.h —
# a 1-line stub of <onnxruntime_cxx_api.h> declaring the opaque Ort types is
# enough to compile and RUN 8 of the 9 test targets. Only test_inference
# actually runs an ORT session; it stays Jetson-only. The stub lives strictly
# inside this script's build dir — the "ONNX is never optional" decision for
# the real build is untouched.
#
# Requirements (ubuntu-24.04): g++ pkg-config libopencv-dev catch2
# nlohmann-json3-dev libspdlog-dev libfmt-dev, and optionally qt6-base-dev
# (unlocks test_tracking_worker + test_dataset_creator via manual moc).
#
# Usage: scripts/ci_unit_tests.sh [build-dir]   (default: build-ci)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build-ci}"
mkdir -p "$BUILD/stub" "$BUILD/bin"

# ── ONNX Runtime stub: only the opaque Ort types named in InferenceEngine.h ──
cat > "$BUILD/stub/onnxruntime_cxx_api.h" <<'EOF'
// CI stub — see scripts/ci_unit_tests.sh. Never used by the real build.
#pragma once
namespace Ort { class Env; class SessionOptions; class Session; class Value; }
EOF

# Third-party includes go through -isystem so their C++20 deprecation noise
# (OpenCV enum arithmetic) doesn't drown our own -Wall output.
sysflags=()
for f in $(pkg-config --cflags-only-I opencv4 catch2 spdlog 2>/dev/null); do
    sysflags+=("-isystem" "${f#-I}")
done
CXXFLAGS=(-std=c++20 -O1 -Wall -fPIC "-I$ROOT/src" "-isystem" "$BUILD/stub" "${sysflags[@]}")
OPENCV_LIBS=$(pkg-config --libs opencv4)
CATCH_LIBS="-lCatch2Main -lCatch2"
LOG_LIBS="-lspdlog -lfmt"

declare -a RESULTS=()
fail=0

build_and_run() {
    local name="$1"; shift
    echo "::group::build $name"
    if g++ "${CXXFLAGS[@]}" "$@" -o "$BUILD/bin/$name"; then
        echo "::endgroup::"
        echo "::group::run $name"
        if "$BUILD/bin/$name"; then
            RESULTS+=("PASS  $name")
        else
            RESULTS+=("FAIL  $name (tests)")
            fail=1
        fi
        echo "::endgroup::"
    else
        echo "::endgroup::"
        RESULTS+=("FAIL  $name (build)")
        fail=1
    fi
}

S="$ROOT/src"; T="$ROOT/tests"

build_and_run test_ibom_parser \
    "$T/test_ibom_parser.cpp" "$S/ibom/IBomParser.cpp" \
    $LOG_LIBS $CATCH_LIBS

build_and_run test_homography \
    "$T/test_homography.cpp" "$S/overlay/Homography.cpp" \
    $OPENCV_LIBS $LOG_LIBS $CATCH_LIBS

build_and_run test_letterbox \
    "$T/test_letterbox.cpp" \
    $OPENCV_LIBS $CATCH_LIBS

build_and_run test_reanchor_gate \
    "$T/test_reanchor_gate.cpp" "$S/overlay/ReanchorGate.cpp" \
    $OPENCV_LIBS $CATCH_LIBS

build_and_run test_boardlocator \
    "$T/test_boardlocator.cpp" "$S/overlay/BoardLocator.cpp" \
    "$S/overlay/Homography.cpp" \
    $OPENCV_LIBS $LOG_LIBS $CATCH_LIBS

build_and_run test_component_matching \
    "$T/test_component_matching.cpp" "$S/ibom/ComponentMap.cpp" \
    $CATCH_LIBS

build_and_run test_project_diff \
    "$T/test_project_diff.cpp" "$S/ibom/ProjectDiff.cpp" \
    $CATCH_LIBS

build_and_run test_alignment_math \
    "$T/test_alignment_math.cpp" "$S/overlay/AlignmentMath.cpp" \
    $OPENCV_LIBS $CATCH_LIBS

build_and_run test_annotations \
    "$T/test_annotations.cpp" "$S/features/Annotations.cpp" \
    $LOG_LIBS $CATCH_LIBS

build_and_run test_board_library \
    "$T/test_board_library.cpp" "$S/features/BoardLibrary.cpp" \
    $LOG_LIBS $CATCH_LIBS

build_and_run test_board_mosaic \
    "$T/test_board_mosaic.cpp" "$S/features/BoardMosaic.cpp" \
    $OPENCV_LIBS $CATCH_LIBS

build_and_run test_golden_diff \
    "$T/test_golden_diff.cpp" "$S/features/GoldenDiff.cpp" \
    "$S/features/BoardMosaic.cpp" \
    $OPENCV_LIBS $CATCH_LIBS

build_and_run test_depth_inspector \
    "$T/test_depth_inspector.cpp" "$S/features/DepthInspector.cpp" \
    $OPENCV_LIBS $CATCH_LIBS

build_and_run test_scene_quality \
    "$T/test_scene_quality.cpp" "$S/utils/SceneQuality.cpp" \
    $OPENCV_LIBS $CATCH_LIBS

build_and_run test_component_reanchor \
    "$T/test_component_reanchor.cpp" "$S/overlay/ComponentReanchor.cpp" \
    "$S/overlay/Homography.cpp" \
    $OPENCV_LIBS $LOG_LIBS $CATCH_LIBS

build_and_run test_blob_detector \
    "$T/test_blob_detector.cpp" "$S/overlay/BlobComponentDetector.cpp" \
    "$S/overlay/ComponentReanchor.cpp" "$S/overlay/Homography.cpp" \
    $OPENCV_LIBS $LOG_LIBS $CATCH_LIBS

# CPU fallback path only (IBOM_USE_UMA_ALLOCATOR undefined — no CUDA here).
build_and_run test_unified_allocator \
    "$T/test_unified_allocator.cpp" "$S/camera/UnifiedAllocator.cpp" \
    $OPENCV_LIBS $LOG_LIBS $CATCH_LIBS

# ── Qt-dependent targets: need qt6-base-dev + a manual moc pass ─────────────
MOC=""
for cand in /usr/lib/qt6/libexec/moc /usr/lib/qt6/bin/moc "$(command -v moc6 || true)"; do
    [ -x "$cand" ] && MOC="$cand" && break
done
if [ -n "$MOC" ] && pkg-config --exists Qt6Core 2>/dev/null; then
    QT_CFLAGS=$(pkg-config --cflags Qt6Core)
    QT_LIBS=$(pkg-config --libs Qt6Core)

    # No moc needed (no Q_OBJECT); QPainter runs on the offscreen platform.
    build_and_run test_overlay_renderer \
        "$T/test_overlay_renderer.cpp" "$S/overlay/OverlayRenderer.cpp" \
        $(pkg-config --cflags Qt6Gui) $(pkg-config --libs Qt6Gui) \
        $OPENCV_LIBS $CATCH_LIBS

    build_and_run test_image_wrap \
        "$T/test_image_wrap.cpp" "$S/utils/ImageUtils.cpp" \
        $(pkg-config --cflags Qt6Gui) $(pkg-config --libs Qt6Gui) \
        $OPENCV_LIBS $LOG_LIBS $CATCH_LIBS

    "$MOC" "$S/overlay/TrackingWorker.h" -o "$BUILD/moc_TrackingWorker.cpp" \
        -I"$S" $QT_CFLAGS
    build_and_run test_tracking_worker \
        "$T/test_tracking_worker.cpp" "$S/overlay/TrackingWorker.cpp" \
        "$BUILD/moc_TrackingWorker.cpp" \
        $QT_CFLAGS $QT_LIBS $OPENCV_LIBS $LOG_LIBS $CATCH_LIBS

    "$MOC" "$S/features/PickAndPlace.h" -o "$BUILD/moc_PickAndPlace.cpp" \
        -I"$S" $QT_CFLAGS
    build_and_run test_pickandplace \
        "$T/test_pickandplace.cpp" "$S/features/PickAndPlace.cpp" \
        "$BUILD/moc_PickAndPlace.cpp" \
        $QT_CFLAGS $QT_LIBS $LOG_LIBS $CATCH_LIBS

    "$MOC" "$S/features/DatasetCreator.h" -o "$BUILD/moc_DatasetCreator.cpp" \
        -I"$S" $QT_CFLAGS
    build_and_run test_dataset_creator \
        "$T/test_dataset_creator.cpp" "$S/features/DatasetCreator.cpp" \
        "$BUILD/moc_DatasetCreator.cpp" \
        $QT_CFLAGS $QT_LIBS $OPENCV_LIBS $LOG_LIBS $CATCH_LIBS
else
    echo "Qt6 Core / moc not found — skipping test_tracking_worker and" \
         "test_dataset_creator (install qt6-base-dev to enable)."
    RESULTS+=("SKIP  test_tracking_worker (no Qt6)")
    RESULTS+=("SKIP  test_dataset_creator (no Qt6)")
fi

echo
echo "── CI unit test summary ──────────────────────────"
printf '%s\n' "${RESULTS[@]}"
exit $fail
