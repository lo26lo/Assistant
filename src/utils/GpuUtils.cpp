#include "GpuUtils.h"

#include <spdlog/spdlog.h>

// Conditionally include CUDA headers
#ifdef __has_include
    #if __has_include(<cuda_runtime.h>)
        #include <cuda_runtime.h>
        #define HAS_CUDA 1
    #else
        #define HAS_CUDA 0
    #endif
#else
    #define HAS_CUDA 0
#endif

// Conditionally include TensorRT
#ifdef __has_include
    #if __has_include(<NvInferVersion.h>)
        #include <NvInferVersion.h>
        #define HAS_TENSORRT 1
    #else
        #define HAS_TENSORRT 0
    #endif
#else
    #define HAS_TENSORRT 0
#endif

namespace ibom::utils {

GpuUtils::GpuInfo GpuUtils::detectGpu()
{
    GpuInfo info;

#if HAS_CUDA
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess || deviceCount == 0) {
        spdlog::warn("GpuUtils: No CUDA devices found");
        return info;
    }

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);

    info.name          = prop.name;
    info.totalMemoryMB = prop.totalGlobalMem / (1024 * 1024);
    info.cudaMajor     = prop.major;
    info.cudaMinor     = prop.minor;
    info.smCount       = prop.multiProcessorCount;
    info.tensorCores   = (prop.major >= 7); // Volta+ have tensor cores
    info.available     = true;

    size_t freeMem, totalMem;
    cudaMemGetInfo(&freeMem, &totalMem);
    info.freeMemoryMB = freeMem / (1024 * 1024);
#else
    spdlog::info("GpuUtils: CUDA not available at compile time");
#endif

    return info;
}

void GpuUtils::getMemoryUsage(size_t& usedMB, size_t& totalMB)
{
    usedMB  = 0;
    totalMB = 0;

#if HAS_CUDA
    size_t freeMem, totalMem;
    if (cudaMemGetInfo(&freeMem, &totalMem) == cudaSuccess) {
        totalMB = totalMem / (1024 * 1024);
        usedMB  = (totalMem - freeMem) / (1024 * 1024);
    }
#endif
}

bool GpuUtils::isCudaAvailable()
{
#if HAS_CUDA
    int count = 0;
    return (cudaGetDeviceCount(&count) == cudaSuccess && count > 0);
#else
    return false;
#endif
}

bool GpuUtils::isTensorRTAvailable()
{
#if HAS_TENSORRT
    return true;
#else
    return false;
#endif
}

std::string GpuUtils::cudaVersionString()
{
#if HAS_CUDA
    int version = 0;
    cudaRuntimeGetVersion(&version);
    int major = version / 1000;
    int minor = (version % 1000) / 10;
    return std::to_string(major) + "." + std::to_string(minor);
#else
    return "N/A";
#endif
}

std::string GpuUtils::tensorRTVersionString()
{
#if HAS_TENSORRT
    return std::to_string(NV_TENSORRT_MAJOR) + "." +
           std::to_string(NV_TENSORRT_MINOR) + "." +
           std::to_string(NV_TENSORRT_PATCH);
#else
    return "N/A";
#endif
}

void GpuUtils::warmUp()
{
#if HAS_CUDA
    // Allocate and free a small buffer to initialize CUDA context
    void* ptr = nullptr;
    cudaError_t err = cudaMalloc(&ptr, 1024);
    if (err == cudaSuccess) {
        cudaMemset(ptr, 0, 1024);
        cudaFree(ptr);
        cudaDeviceSynchronize();
        spdlog::info("GpuUtils: GPU warmup complete");
    } else {
        spdlog::warn("GpuUtils: GPU warmup failed: {}", cudaGetErrorString(err));
    }
#endif
}

void GpuUtils::logGpuInfo()
{
    auto info = detectGpu();
    if (info.available) {
        spdlog::info("GPU: {} ({} MB, CUDA {}.{}, {} SMs, TensorCores: {})",
                     info.name, info.totalMemoryMB,
                     info.cudaMajor, info.cudaMinor, info.smCount,
                     info.tensorCores ? "yes" : "no");
        spdlog::info("GPU Memory: {} MB free / {} MB total",
                     info.freeMemoryMB, info.totalMemoryMB);
        spdlog::info("CUDA: {}, TensorRT: {}",
                     cudaVersionString(), tensorRTVersionString());
    } else {
        spdlog::warn("No GPU detected — running in CPU-only mode");
    }
}

} // namespace ibom::utils
