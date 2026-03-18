#include "FrameBuffer.h"

#include <chrono>

namespace ibom::camera {

FrameBuffer::FrameBuffer(size_t capacity)
    : m_capacity(capacity)
    , m_buffer(capacity)
{
}

void FrameBuffer::push(const cv::Mat& frame)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    frame.copyTo(m_buffer[m_head]);
    m_totalFrames++;

    if (m_count == m_capacity) {
        // Buffer full — overwrite oldest (drop it)
        m_tail = (m_tail + 1) % m_capacity;
        m_droppedFrames++;
    } else {
        m_count++;
    }

    m_head = (m_head + 1) % m_capacity;
    m_cond.notify_one();
}

cv::Mat FrameBuffer::pop(uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (timeout_ms == 0) {
        m_cond.wait(lock, [this] { return m_count > 0; });
    } else {
        if (!m_cond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return m_count > 0; })) {
            return {}; // Timeout
        }
    }

    cv::Mat result = m_buffer[m_tail].clone();
    m_tail = (m_tail + 1) % m_capacity;
    m_count--;

    return result;
}

bool FrameBuffer::tryPop(cv::Mat& frame)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_count == 0) return false;

    frame = m_buffer[m_tail].clone();
    m_tail = (m_tail + 1) % m_capacity;
    m_count--;

    return true;
}

size_t FrameBuffer::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_count;
}

bool FrameBuffer::empty() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_count == 0;
}

void FrameBuffer::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_head = 0;
    m_tail = 0;
    m_count = 0;
}

} // namespace ibom::camera
