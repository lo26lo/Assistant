#pragma once

#include <string>
#include <cstddef>

namespace ibom::utils {

/// GPU detection, memory monitoring, and CUDA utilities
class GpuUtils {
public:
    struct GpuInfo {
        std::string name;
        size_t      totalMemoryMB = 0;
        size_t      freeMemoryMB  = 0;
        int         cudaMajor     = 0;
        int         cudaMinor     = 0;
        int         smCount       = 0;          // Streaming multiprocessors
        bool        tensorCores   = false;
        bool        available     = false;
    };

    /// Detect the primary CUDA GPU
    static GpuInfo detectGpu();

    /// Get current GPU memory usage
    static void getMemoryUsage(size_t& usedMB, size_t& totalMB);

    /// Check if CUDA is available
    static bool isCudaAvailable();

    /// Check if TensorRT is available
    static bool isTensorRTAvailable();

    /// Get CUDA version string
    static std::string cudaVersionString();

    /// Get TensorRT version string
    static std::string tensorRTVersionString();

    /// Warm up GPU (run a small kernel to initialize context)
    static void warmUp();

    /// Print GPU info to log
    static void logGpuInfo();
};

} // namespace ibom::utils
