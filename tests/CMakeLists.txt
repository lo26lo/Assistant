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

# Component matching test — needs ComponentMap.cpp (no extra libs)
add_ibom_test(test_component_matching
    SOURCES test_component_matching.cpp
            ${CMAKE_SOURCE_DIR}/src/ibom/ComponentMap.cpp
    LIBS    nlohmann_json::nlohmann_json
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
