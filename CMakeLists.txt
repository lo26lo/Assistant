cmake_minimum_required(VERSION 3.28)

# Fix: CMake 4.x + Ninja on Windows — rules.ninja not generated during try_compile
# Root cause: path normalization issue with short DOS paths (COMMON~1) in CMAKE_MAKE_PROGRAM
# Solution from: https://cmake.org/cmake/help/latest/variable/CMAKE_TRY_COMPILE_TARGET_TYPE.html
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ============================================================================
# MicroscopeIBOM — PCB Inspection with AI Overlay
# ============================================================================
project(MicroscopeIBOM
    VERSION 0.1.0
    DESCRIPTION "Microscope camera with iBOM AI overlay for PCB inspection"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt6 auto-tools
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Build output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================================
# Options
# ============================================================================
option(IBOM_ENABLE_TENSORRT   "Enable TensorRT acceleration"           ON)
option(IBOM_ENABLE_VOICE      "Enable voice control (requires portaudio)" OFF)
option(IBOM_ENABLE_REMOTE     "Enable remote view streaming"           ON)
option(IBOM_ENABLE_TESTS      "Build unit tests"                       ON)
option(IBOM_ENABLE_ASAN       "Enable AddressSanitizer (debug)"        OFF)

# ============================================================================
# Custom CMake modules
# ============================================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ============================================================================
# Dependencies
# ============================================================================

# Qt6
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    OpenGL
    OpenGLWidgets
    Multimedia
    MultimediaWidgets
    WebSockets
    Concurrent
    PrintSupport
)

# OpenCV
find_package(OpenCV REQUIRED COMPONENTS
    core
    imgproc
    highgui
    videoio
    calib3d
    dnn
    features2d
)

# ONNX Runtime
find_package(onnxruntime CONFIG REQUIRED)

# nlohmann/json
find_package(nlohmann_json CONFIG REQUIRED)

# spdlog
find_package(spdlog CONFIG REQUIRED)

# ZXing (barcode/QR)
find_package(ZXing CONFIG REQUIRED)

# libharu (PDF generation)
find_package(unofficial-libharu CONFIG QUIET)
if(NOT unofficial-libharu_FOUND)
    find_package(libharu CONFIG QUIET)
    if(NOT libharu_FOUND)
        find_package(HPDF QUIET)
    endif()
endif()

# CUDA (optional, for GPU utilities)
find_package(CUDAToolkit QUIET)

# TensorRT (optional)
if(IBOM_ENABLE_TENSORRT)
    include(FindTensorRT)
endif()

# ============================================================================
# Source files
# ============================================================================
set(SOURCES
    # App
    src/main.cpp
    src/app/Application.cpp
    src/app/Config.cpp

    # Camera
    src/camera/CameraCapture.cpp
    src/camera/CameraCalibration.cpp
    src/camera/FrameBuffer.cpp

    # iBOM
    src/ibom/IBomParser.cpp
    src/ibom/IBomData.cpp
    src/ibom/ComponentMap.cpp

    # AI
    src/ai/InferenceEngine.cpp
    src/ai/ComponentDetector.cpp
    src/ai/SolderInspector.cpp
    src/ai/OCREngine.cpp
    src/ai/ModelManager.cpp

    # Overlay
    src/overlay/OverlayRenderer.cpp
    src/overlay/Homography.cpp
    src/overlay/ComponentOverlay.cpp
    src/overlay/HeatmapRenderer.cpp

    # GUI
    src/gui/MainWindow.cpp
    src/gui/CameraView.cpp
    src/gui/BomPanel.cpp
    src/gui/ControlPanel.cpp
    src/gui/InspectionWizard.cpp
    src/gui/StatsPanel.cpp
    src/gui/SettingsDialog.cpp

    # Features
    src/features/PickAndPlace.cpp
    src/features/VoiceControl.cpp
    src/features/BarcodeScanner.cpp
    src/features/Measurement.cpp
    src/features/StencilAlign.cpp
    src/features/SnapshotHistory.cpp
    src/features/RemoteView.cpp

    # Export
    src/export/ReportGenerator.cpp
    src/export/DataExporter.cpp

    # Utils
    src/utils/Logger.cpp
    src/utils/GpuUtils.cpp
    src/utils/ImageUtils.cpp
)

set(HEADERS
    # App
    src/app/Application.h
    src/app/Config.h

    # Camera
    src/camera/CameraCapture.h
    src/camera/CameraCalibration.h
    src/camera/FrameBuffer.h

    # iBOM
    src/ibom/IBomParser.h
    src/ibom/IBomData.h
    src/ibom/ComponentMap.h

    # AI
    src/ai/InferenceEngine.h
    src/ai/ComponentDetector.h
    src/ai/SolderInspector.h
    src/ai/OCREngine.h
    src/ai/ModelManager.h

    # Overlay
    src/overlay/OverlayRenderer.h
    src/overlay/Homography.h
    src/overlay/ComponentOverlay.h
    src/overlay/HeatmapRenderer.h

    # GUI
    src/gui/MainWindow.h
    src/gui/CameraView.h
    src/gui/BomPanel.h
    src/gui/ControlPanel.h
    src/gui/InspectionWizard.h
    src/gui/StatsPanel.h
    src/gui/SettingsDialog.h
    src/gui/Theme.h

    # Features
    src/features/PickAndPlace.h
    src/features/VoiceControl.h
    src/features/BarcodeScanner.h
    src/features/Measurement.h
    src/features/StencilAlign.h
    src/features/SnapshotHistory.h
    src/features/RemoteView.h

    # Export
    src/export/ReportGenerator.h
    src/export/DataExporter.h

    # Utils
    src/utils/Logger.h
    src/utils/GpuUtils.h
    src/utils/ImageUtils.h
)

# ============================================================================
# Resources
# ============================================================================
set(RESOURCES
    resources/resources.qrc
)

# ============================================================================
# Executable
# ============================================================================
qt_add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
    ${RESOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# ============================================================================
# Link libraries
# ============================================================================
target_link_libraries(${PROJECT_NAME} PRIVATE
    # Qt6
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::WebSockets
    Qt6::Concurrent
    Qt6::PrintSupport

    # OpenCV
    ${OpenCV_LIBS}

    # ONNX Runtime
    onnxruntime::onnxruntime

    # JSON
    nlohmann_json::nlohmann_json

    # Logging
    spdlog::spdlog

    # Barcode
    ZXing::ZXing
)

# Optional: libharu
if(unofficial-libharu_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE unofficial::libharu::hpdf)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IBOM_HAS_LIBHARU)
elseif(libharu_FOUND OR HPDF_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE hpdf)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IBOM_HAS_LIBHARU)
endif()

# Optional: CUDA
if(CUDAToolkit_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE CUDA::cudart)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IBOM_HAS_CUDA)
endif()

# ============================================================================
# Compiler flags (must be after target creation)
# ============================================================================
include(CompilerFlags)

# Optional: TensorRT
if(IBOM_ENABLE_TENSORRT AND TensorRT_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${TensorRT_LIBRARIES})
    target_include_directories(${PROJECT_NAME} PRIVATE ${TensorRT_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE IBOM_HAS_TENSORRT)
endif()

# ============================================================================
# Platform-specific
# ============================================================================
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IBOM_PLATFORM_WINDOWS)
    # Use WinMain on Windows
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
elseif(UNIX)
    target_compile_definitions(${PROJECT_NAME} PRIVATE IBOM_PLATFORM_LINUX)
endif()

# ============================================================================
# Tests
# ============================================================================
if(IBOM_ENABLE_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

# ============================================================================
# Install
# ============================================================================
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)

install(DIRECTORY models/ DESTINATION share/${PROJECT_NAME}/models
    FILES_MATCHING PATTERN "*.onnx"
)

install(DIRECTORY resources/ DESTINATION share/${PROJECT_NAME}/resources
    FILES_MATCHING
    PATTERN "*.png"
    PATTERN "*.svg"
    PATTERN "*.qml"
)

# ============================================================================
# Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== MicroscopeIBOM Configuration ===")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt6:          ${Qt6_VERSION}")
message(STATUS "  OpenCV:       ${OpenCV_VERSION}")
message(STATUS "  CUDA:         ${CUDAToolkit_FOUND}")
message(STATUS "  TensorRT:     ${IBOM_ENABLE_TENSORRT}")
message(STATUS "  Tests:        ${IBOM_ENABLE_TESTS}")
message(STATUS "  Voice:        ${IBOM_ENABLE_VOICE}")
message(STATUS "  Remote:       ${IBOM_ENABLE_REMOTE}")
message(STATUS "====================================")
message(STATUS "")
