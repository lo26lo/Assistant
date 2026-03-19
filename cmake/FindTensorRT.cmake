# FindTensorRT.cmake
# Locate NVIDIA TensorRT library
#
# Sets:
#   TensorRT_FOUND
#   TensorRT_INCLUDE_DIRS
#   TensorRT_LIBRARIES
#   TensorRT_VERSION

# Common install paths
set(_TRT_SEARCH_PATHS
    "$ENV{TENSORRT_HOME}"
    "C:/TensorRT/TensorRT-10.15.1.29"
    "C:/Tools/TensorRT"
    "C:/Program Files/NVIDIA GPU Computing Toolkit/TensorRT"
    "/usr/lib/x86_64-linux-gnu"
    "/usr/local/tensorrt"
    "$ENV{TENSORRT_ROOT}"
    "$ENV{TRT_ROOT}"
)

# Find include directory
find_path(TensorRT_INCLUDE_DIR
    NAMES NvInfer.h
    PATHS ${_TRT_SEARCH_PATHS}
    PATH_SUFFIXES include
)

# Find libraries (TensorRT 10.x uses versioned names like nvinfer_10)
find_library(TensorRT_nvinfer
    NAMES nvinfer nvinfer_10
    PATHS ${_TRT_SEARCH_PATHS}
    PATH_SUFFIXES lib lib64 lib/x64
)

find_library(TensorRT_nvinfer_plugin
    NAMES nvinfer_plugin nvinfer_plugin_10
    PATHS ${_TRT_SEARCH_PATHS}
    PATH_SUFFIXES lib lib64 lib/x64
)

find_library(TensorRT_nvonnxparser
    NAMES nvonnxparser nvonnxparser_10
    PATHS ${_TRT_SEARCH_PATHS}
    PATH_SUFFIXES lib lib64 lib/x64
)

# Extract version from header
if(TensorRT_INCLUDE_DIR)
    file(STRINGS "${TensorRT_INCLUDE_DIR}/NvInferVersion.h" _TRT_VER_MAJOR
        REGEX "#define NV_TENSORRT_MAJOR [0-9]+")
    file(STRINGS "${TensorRT_INCLUDE_DIR}/NvInferVersion.h" _TRT_VER_MINOR
        REGEX "#define NV_TENSORRT_MINOR [0-9]+")
    file(STRINGS "${TensorRT_INCLUDE_DIR}/NvInferVersion.h" _TRT_VER_PATCH
        REGEX "#define NV_TENSORRT_PATCH [0-9]+")

    if(_TRT_VER_MAJOR AND _TRT_VER_MINOR AND _TRT_VER_PATCH)
        string(REGEX REPLACE ".*([0-9]+)" "\\1" TensorRT_VERSION_MAJOR "${_TRT_VER_MAJOR}")
        string(REGEX REPLACE ".*([0-9]+)" "\\1" TensorRT_VERSION_MINOR "${_TRT_VER_MINOR}")
        string(REGEX REPLACE ".*([0-9]+)" "\\1" TensorRT_VERSION_PATCH "${_TRT_VER_PATCH}")
        set(TensorRT_VERSION "${TensorRT_VERSION_MAJOR}.${TensorRT_VERSION_MINOR}.${TensorRT_VERSION_PATCH}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TensorRT
    REQUIRED_VARS TensorRT_INCLUDE_DIR TensorRT_nvinfer
    VERSION_VAR TensorRT_VERSION
)

if(TensorRT_FOUND)
    set(TensorRT_INCLUDE_DIRS ${TensorRT_INCLUDE_DIR})
    set(TensorRT_LIBRARIES
        ${TensorRT_nvinfer}
        ${TensorRT_nvinfer_plugin}
        ${TensorRT_nvonnxparser}
    )
    message(STATUS "Found TensorRT ${TensorRT_VERSION}: ${TensorRT_INCLUDE_DIRS}")
endif()

mark_as_advanced(
    TensorRT_INCLUDE_DIR
    TensorRT_nvinfer
    TensorRT_nvinfer_plugin
    TensorRT_nvonnxparser
)
