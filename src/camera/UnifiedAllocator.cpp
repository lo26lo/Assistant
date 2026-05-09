#include "UnifiedAllocator.h"

#include <spdlog/spdlog.h>
#include <atomic>
#include <cstdlib>
#include <new>

#ifdef IBOM_USE_UMA_ALLOCATOR
  #include <cuda_runtime.h>
#endif

namespace ibom::camera {

namespace {

#ifdef IBOM_USE_UMA_ALLOCATOR
// Set to false the first time cudaMallocManaged fails so subsequent allocations
// silently fall back to std::malloc without spamming the log.
std::atomic<bool> g_umaActive{true};

bool probeUma()
{
    void* p = nullptr;
    cudaError_t err = cudaMallocManaged(&p, 64);
    if (err != cudaSuccess) {
        spdlog::warn("UnifiedAllocator: cudaMallocManaged probe failed ({}); "
                     "falling back to std::malloc",
                     cudaGetErrorString(err));
        g_umaActive.store(false, std::memory_order_relaxed);
        return false;
    }
    cudaFree(p);
    spdlog::info("UnifiedAllocator: CUDA Unified Memory active");
    return true;
}

void* allocateRaw(size_t size)
{
    if (g_umaActive.load(std::memory_order_relaxed)) {
        void* p = nullptr;
        cudaError_t err = cudaMallocManaged(&p, size);
        if (err == cudaSuccess) return p;
        // Permanently disable to avoid log spam; further allocs go to malloc.
        if (g_umaActive.exchange(false, std::memory_order_relaxed)) {
            spdlog::warn("UnifiedAllocator: cudaMallocManaged failed ({}); "
                         "switching to std::malloc for the rest of the session",
                         cudaGetErrorString(err));
        }
    }
    return std::malloc(size);
}

void deallocateRaw(void* ptr)
{
    if (!ptr) return;
    // We cannot tell which API allocated this pointer once UMA flips off
    // mid-session. Probe with cudaPointerGetAttributes — managed pointers
    // report cudaMemoryTypeManaged, malloc'd pointers report
    // cudaMemoryTypeUnregistered (or the call returns an error).
    cudaPointerAttributes attr{};
    cudaError_t err = cudaPointerGetAttributes(&attr, ptr);
    if (err == cudaSuccess && attr.type == cudaMemoryTypeManaged) {
        cudaFree(ptr);
    } else {
        // Reset CUDA's sticky error from the unregistered pointer probe.
        cudaGetLastError();
        std::free(ptr);
    }
}

#else // IBOM_USE_UMA_ALLOCATOR

void* allocateRaw(size_t size) { return std::malloc(size); }
void deallocateRaw(void* ptr)  { std::free(ptr); }

#endif

class UnifiedMatAllocator final : public cv::MatAllocator {
public:
    cv::UMatData* allocate(int dims, const int* sizes, int type,
                           void* data0, size_t* step,
                           cv::AccessFlag /*flags*/,
                           cv::UMatUsageFlags /*usageFlags*/) const override
    {
        // Compute total byte size and fill step[] (mirrors the logic of the
        // default OpenCV StdAllocator).
        size_t total = CV_ELEM_SIZE(type);
        for (int i = dims - 1; i >= 0; --i) {
            if (step) {
                if (data0 && step[i] != CV_AUTOSTEP) {
                    CV_Assert(total <= step[i]);
                    total = step[i];
                } else {
                    step[i] = total;
                }
            }
            total *= static_cast<size_t>(sizes[i]);
        }

        uint8_t* data = static_cast<uint8_t*>(data0);
        if (!data) {
            data = static_cast<uint8_t*>(allocateRaw(total));
            if (!data) throw std::bad_alloc();
        }

        auto* u = new cv::UMatData(this);
        u->data = u->origdata = data;
        u->size = total;
        if (data0) {
            u->flags |= cv::UMatData::USER_ALLOCATED;
        }
        return u;
    }

    bool allocate(cv::UMatData* u, cv::AccessFlag /*flags*/,
                  cv::UMatUsageFlags /*usageFlags*/) const override
    {
        // No CPU/GPU mirror needed — we're already shared memory. Just confirm
        // the storage exists.
        return u != nullptr;
    }

    void deallocate(cv::UMatData* u) const override
    {
        if (!u) return;
        if (!(u->flags & cv::UMatData::USER_ALLOCATED) && u->origdata) {
            deallocateRaw(u->origdata);
        }
        delete u;
    }
};

UnifiedMatAllocator& instance()
{
    static UnifiedMatAllocator alloc;
#ifdef IBOM_USE_UMA_ALLOCATOR
    static const bool kProbed = probeUma();
    (void)kProbed;
#endif
    return alloc;
}

} // namespace

cv::MatAllocator* unifiedAllocator()
{
    return &instance();
}

bool unifiedMemoryAvailable()
{
#ifdef IBOM_USE_UMA_ALLOCATOR
    (void)instance(); // ensure probe ran
    return g_umaActive.load(std::memory_order_relaxed);
#else
    return false;
#endif
}

} // namespace ibom::camera
