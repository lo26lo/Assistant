#pragma once

#include <opencv2/core.hpp>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <cstdint>

namespace ibom::camera {

/**
 * @brief Lock-free-ish circular frame buffer for low-latency
 *        producer/consumer pattern (camera → AI pipeline).
 *
 * Uses a fixed-size ring buffer with mutex protection.
 * The producer (camera thread) always overwrites the oldest frame
 * if the buffer is full — we never want to stall the camera.
 */
class FrameBuffer {
public:
    /// @param capacity Number of frame slots (e.g., 3 = triple buffer).
    explicit FrameBuffer(size_t capacity = 3);
    ~FrameBuffer() = default;

    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    /// Push a frame (producer side). Overwrites oldest if full.
    void push(const cv::Mat& frame);

    /// Pop the oldest frame (consumer side). Blocks until available.
    /// @param timeout_ms Maximum wait time; 0 = infinite.
    /// @return The frame, or empty Mat on timeout.
    cv::Mat pop(uint32_t timeout_ms = 0);

    /// Try to pop without blocking.
    /// @return true if a frame was available.
    bool tryPop(cv::Mat& frame);

    /// Number of frames currently buffered.
    size_t size() const;

    /// Whether the buffer is empty.
    bool empty() const;

    /// Clear all buffered frames.
    void clear();

    /// Get total frames pushed since creation.
    uint64_t totalFrames() const { return m_totalFrames; }

    /// Get total frames dropped (overwritten before consumed).
    uint64_t droppedFrames() const { return m_droppedFrames; }

private:
    size_t m_capacity;
    std::vector<cv::Mat> m_buffer;
    size_t m_head = 0; // Write position
    size_t m_tail = 0; // Read position
    size_t m_count = 0;

    mutable std::mutex m_mutex;
    std::condition_variable m_cond;

    uint64_t m_totalFrames = 0;
    uint64_t m_droppedFrames = 0;
};

} // namespace ibom::camera
