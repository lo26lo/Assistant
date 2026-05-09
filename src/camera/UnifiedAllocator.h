#pragma once

#include <opencv2/core.hpp>
#include <cstddef>

namespace ibom::camera {

/**
 * @brief Custom OpenCV allocator that backs cv::Mat pixel storage with
 *        CUDA Unified Memory (cudaMallocManaged) when available.
 *
 * On Jetson (Tegra SoC) the GPU and CPU share physical memory. Allocating
 * frames via cudaMallocManaged lets the same buffer be read by V4L2/OpenCV
 * on the CPU and by TensorRT/CUDA kernels on the GPU without any host↔device
 * copies — the OS migrates pages on access (or, on Tegra, simply maps them
 * to both spaces with zero migration cost).
 *
 * Build-time switch:
 *   - If IBOM_USE_UMA_ALLOCATOR is defined (set by CMake when CUDA is found
 *     AND -DIBOM_ENABLE_UMA=ON), pixel buffers go through cudaMallocManaged.
 *     A warning is logged and std::malloc is used if the CUDA call fails.
 *   - Otherwise, std::malloc is used unconditionally (behavior identical to
 *     the OpenCV default allocator). Same wiring on every platform.
 *
 * Usage:
 *   cv::Mat frame;
 *   frame.allocator = ibom::camera::unifiedAllocator();
 *   cap.read(frame); // pixel buffer allocated via UMA when enabled
 *
 * Thread-safety: the returned allocator is a process-wide singleton with no
 * mutable state. Safe to call from any thread.
 */
cv::MatAllocator* unifiedAllocator();

/// True when the build is configured for unified memory AND a runtime probe
/// of cudaMallocManaged succeeded. False on Windows / non-CUDA builds.
bool unifiedMemoryAvailable();

} // namespace ibom::camera
