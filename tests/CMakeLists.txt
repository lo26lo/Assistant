cmake_minimum_required(VERSION 3.22)

find_package(Catch2 3 REQUIRED)

# Helper: add a test target with per-test sources and libraries
function(add_ibom_test NAME)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${NAME} ${ARG_SOURCES})
    target_link_libraries(${NAME} PRIVATE
        Catch2::Catch2WithMain
        ${ARG_LIBS}
    )
    target_include_directories(${NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(${NAME} PRIVATE cxx_std_20)
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

# IBom parser test — needs IBomParser.cpp (depends on spdlog, nlohmann_json)
add_ibom_test(test_ibom_parser
    SOURCES test_ibom_parser.cpp
            ${CMAKE_SOURCE_DIR}/src/ibom/IBomParser.cpp
    LIBS    nlohmann_json::nlohmann_json spdlog::spdlog
)

# Homography test — needs Homography.cpp (depends on opencv calib3d, spdlog)
add_ibom_test(test_homography
    SOURCES test_homography.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/Homography.cpp
    LIBS    opencv_core opencv_imgproc opencv_calib3d spdlog::spdlog
)

# Inference test — needs InferenceEngine.cpp + ModelManager.cpp (onnxruntime, opencv, spdlog)
add_ibom_test(test_inference
    SOURCES test_inference.cpp
            ${CMAKE_SOURCE_DIR}/src/ai/InferenceEngine.cpp
            ${CMAKE_SOURCE_DIR}/src/ai/ModelManager.cpp
    LIBS    opencv_core opencv_imgproc opencv_dnn
            onnxruntime::onnxruntime spdlog::spdlog
)

# Component re-anchor test — prior-free bootstrap registration (pure
# geometry, synthetic detections). ComponentReanchor.h pulls ai::Detection
# from InferenceEngine.h, which includes the onnxruntime headers → link it
# like test_inference does. Homography.cpp provides the prior/refine path.
add_ibom_test(test_component_reanchor
    SOURCES test_component_reanchor.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/ComponentReanchor.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/Homography.cpp
    LIBS    opencv_core opencv_imgproc opencv_calib3d
            onnxruntime::onnxruntime spdlog::spdlog
)

# Blob component detector test — model-free detection + bootstrap end-to-end
# (synthetic PCB image). Needs features2d (MSER) + the reanchor/homography src.
add_ibom_test(test_blob_detector
    SOURCES test_blob_detector.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/BlobComponentDetector.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/ComponentReanchor.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/Homography.cpp
    LIBS    opencv_core opencv_imgproc opencv_features2d opencv_calib3d
            onnxruntime::onnxruntime spdlog::spdlog
)

# Re-anchor gate test — the silent-correction decision logic (drift gate,
# two-tick confirmation, Lost bypass). Pure geometry, no Qt.
add_ibom_test(test_reanchor_gate
    SOURCES test_reanchor_gate.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/ReanchorGate.cpp
    LIBS    opencv_core
)

# BoardLocator test — synthetic contour/depth scenes with a known pose.
add_ibom_test(test_boardlocator
    SOURCES test_boardlocator.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/BoardLocator.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/Homography.cpp
    LIBS    opencv_core opencv_imgproc opencv_calib3d spdlog::spdlog
)

# Letterbox test — pure geometry (header-only src/ai/Letterbox.h), no ONNX.
add_ibom_test(test_letterbox
    SOURCES test_letterbox.cpp
    LIBS    opencv_core opencv_imgproc
)

# Board mosaic test — the board-scan accumulation core (A1). Pure OpenCV.
add_ibom_test(test_board_mosaic
    SOURCES test_board_mosaic.cpp
            ${CMAKE_SOURCE_DIR}/src/features/BoardMosaic.cpp
    LIBS    opencv_core opencv_imgproc
)

# Golden diff test — tile dissimilarity + per-component scoring (A2).
add_ibom_test(test_golden_diff
    SOURCES test_golden_diff.cpp
            ${CMAKE_SOURCE_DIR}/src/features/GoldenDiff.cpp
            ${CMAKE_SOURCE_DIR}/src/features/BoardMosaic.cpp
    LIBS    opencv_core opencv_imgproc
)

# Depth inspector test — plane fit + presence verdicts (A3). Pure OpenCV.
add_ibom_test(test_depth_inspector
    SOURCES test_depth_inspector.cpp
            ${CMAKE_SOURCE_DIR}/src/features/DepthInspector.cpp
    LIBS    opencv_core opencv_imgproc opencv_calib3d
)

# Scene quality test — the scene advisor's analysis core (D1). Pure OpenCV.
add_ibom_test(test_scene_quality
    SOURCES test_scene_quality.cpp
            ${CMAKE_SOURCE_DIR}/src/utils/SceneQuality.cpp
    LIBS    opencv_core opencv_imgproc
)

# Revision diff test — update-rework classification (C1). Pure std logic;
# nlohmann_json only because IBomData.h includes it.
add_ibom_test(test_project_diff
    SOURCES test_project_diff.cpp
            ${CMAKE_SOURCE_DIR}/src/ibom/ProjectDiff.cpp
    LIBS    nlohmann_json::nlohmann_json
)

# Component matching test — needs ComponentMap.cpp (no extra libs)
add_ibom_test(test_component_matching
    SOURCES test_component_matching.cpp
            ${CMAKE_SOURCE_DIR}/src/ibom/ComponentMap.cpp
    LIBS    nlohmann_json::nlohmann_json
)

# Alignment similarity math — the helper shared by the 4 similarity alignment
# paths (anchor click, minimap anchor, 2-comp, multi n=2). Pure OpenCV.
add_ibom_test(test_alignment_math
    SOURCES test_alignment_math.cpp
            ${CMAKE_SOURCE_DIR}/src/overlay/AlignmentMath.cpp
    LIBS    opencv_core opencv_calib3d
)

# Component annotation store (B2) — pure std + nlohmann.
add_ibom_test(test_annotations
    SOURCES test_annotations.cpp
            ${CMAKE_SOURCE_DIR}/src/features/Annotations.cpp
    LIBS    nlohmann_json::nlohmann_json spdlog::spdlog
)

# Board library registry (C2) — pure std + nlohmann.
add_ibom_test(test_board_library
    SOURCES test_board_library.cpp
            ${CMAKE_SOURCE_DIR}/src/features/BoardLibrary.cpp
    LIBS    nlohmann_json::nlohmann_json spdlog::spdlog
)

# Unified allocator test — needs UnifiedAllocator.cpp (depends on opencv core,
# spdlog, and CUDA when IBOM_USE_UMA_ALLOCATOR is propagated from the parent).
add_ibom_test(test_unified_allocator
    SOURCES test_unified_allocator.cpp
            ${CMAKE_SOURCE_DIR}/src/camera/UnifiedAllocator.cpp
    LIBS    opencv_core opencv_imgproc spdlog::spdlog
)
# Mirror the parent's UMA define so the allocator picks the right backend.
get_target_property(_ibom_defs MicroscopeIBOM COMPILE_DEFINITIONS)
if(_ibom_defs)
    foreach(_d IN LISTS _ibom_defs)
        if(_d STREQUAL "IBOM_USE_UMA_ALLOCATOR")
            target_compile_definitions(test_unified_allocator PRIVATE IBOM_USE_UMA_ALLOCATOR)
            target_link_libraries(test_unified_allocator PRIVATE CUDA::cudart)
        endif()
    endforeach()
endif()

# Tracking worker test — TrackingWorker is a QObject (needs MOC) and uses ORB +
# BFMatcher + RANSAC from opencv features2d/calib3d. Built as its own target
# with AUTOMOC + Qt6::Core (CameraCapture.h, pulled in for FrameRef, only needs
# Core: QObject/QMetaType/QSize). Guarded so a Qt-less host just skips it.
find_package(Qt6 QUIET COMPONENTS Core Gui)
if(Qt6_FOUND)
    # Overlay renderer test — board-space buffer, active-layer filter and the
    # mirrored back-side view. QPainter on QImage runs headless (the test
    # boots a QGuiApplication on the offscreen platform). No Q_OBJECT → no moc.
    add_ibom_test(test_overlay_renderer
        SOURCES test_overlay_renderer.cpp
                ${CMAKE_SOURCE_DIR}/src/overlay/OverlayRenderer.cpp
        LIBS    Qt6::Gui opencv_core
    )

    # Zero-copy Mat→QImage wraps (display hot path). Pure QImage data ops —
    # no app instance, no moc.
    add_ibom_test(test_image_wrap
        SOURCES test_image_wrap.cpp
                ${CMAKE_SOURCE_DIR}/src/utils/ImageUtils.cpp
        LIBS    Qt6::Gui opencv_core opencv_imgproc opencv_imgcodecs
                spdlog::spdlog
    )
    add_executable(test_tracking_worker
        test_tracking_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/overlay/TrackingWorker.cpp
    )
    set_target_properties(test_tracking_worker PROPERTIES AUTOMOC ON)
    target_include_directories(test_tracking_worker PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(test_tracking_worker PRIVATE cxx_std_20)
    # Link the full OpenCV set found at the root (same as the main target) so
    # the Phase 3 tracking code resolves: opencv_video (calcOpticalFlowPyrLK)
    # and, when IBOM_HAVE_OPENCV_CUDA is defined, the optional CUDA modules
    # (cv::cuda::ORB). The previous explicit subset omitted both.
    target_link_libraries(test_tracking_worker PRIVATE
        Catch2::Catch2WithMain
        Qt6::Core
        ${OpenCV_LIBS}
        spdlog::spdlog
    )
    add_test(NAME test_tracking_worker COMMAND test_tracking_worker)
else()
    message(STATUS "Qt6 Core not found — skipping test_tracking_worker")
endif()

# PickAndPlace test — nearest-neighbor route + unplace (B1/B3). QObject
# (AUTOMOC), Qt6::Core is enough.
if(Qt6_FOUND)
    add_executable(test_pickandplace
        test_pickandplace.cpp
        ${CMAKE_SOURCE_DIR}/src/features/PickAndPlace.cpp
    )
    set_target_properties(test_pickandplace PROPERTIES AUTOMOC ON)
    target_include_directories(test_pickandplace PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(test_pickandplace PRIVATE cxx_std_20)
    target_link_libraries(test_pickandplace PRIVATE
        Catch2::Catch2WithMain
        Qt6::Core
        spdlog::spdlog
    )
    add_test(NAME test_pickandplace COMMAND test_pickandplace)
endif()

# Dataset creator test — class mapping + YOLO label projection. DatasetCreator
# is a QObject (AUTOMOC) but the tests only exercise the pure helpers; Qt6::Core
# is enough (QObject/QString/QDateTime).
if(Qt6_FOUND)
    add_executable(test_dataset_creator
        test_dataset_creator.cpp
        ${CMAKE_SOURCE_DIR}/src/features/DatasetCreator.cpp
    )
    set_target_properties(test_dataset_creator PROPERTIES AUTOMOC ON)
    target_include_directories(test_dataset_creator PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(test_dataset_creator PRIVATE cxx_std_20)
    target_link_libraries(test_dataset_creator PRIVATE
        Catch2::Catch2WithMain
        Qt6::Core
        opencv_core opencv_imgproc opencv_imgcodecs
        nlohmann_json::nlohmann_json
        spdlog::spdlog
    )
    add_test(NAME test_dataset_creator COMMAND test_dataset_creator)
else()
    message(STATUS "Qt6 Core not found — skipping test_dataset_creator")
endif()
