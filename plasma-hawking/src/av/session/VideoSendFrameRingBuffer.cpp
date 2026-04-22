#include "VideoSendFrameRingBuffer.h"

#include <QMutexLocker>

#include <algorithm>

namespace av::session {

VideoSendFrameRingBuffer::VideoSendFrameRingBuffer(std::size_t capacity)
    : m_capacity((std::max)(capacity, std::size_t{1})) {}

void VideoSendFrameRingBuffer::reset() {
    QMutexLocker locker(&m_mutex);
    m_frames.clear();
    m_closed = false;
    m_waitCondition.wakeAll();
}

void VideoSendFrameRingBuffer::close() {
    QMutexLocker locker(&m_mutex);
    m_closed = true;
    m_waitCondition.wakeAll();
}

void VideoSendFrameRingBuffer::clear() {
    QMutexLocker locker(&m_mutex);
    m_frames.clear();
}

bool VideoSendFrameRingBuffer::push(VideoSendCapturedFrame frame) {
    QMutexLocker locker(&m_mutex);
    if (m_closed) {
        return false;
    }
    if (m_frames.size() >= m_capacity) {
        m_frames.pop_front();
    }
    m_frames.push_back(std::move(frame));
    m_waitCondition.wakeOne();
    return true;
}

bool VideoSendFrameRingBuffer::popWait(VideoSendCapturedFrame& outFrame,
                                       std::chrono::milliseconds timeout) {
    QMutexLocker locker(&m_mutex);
    while (m_frames.empty() && !m_closed) {
        const auto timeoutCount = timeout.count();
        const auto waitMs = timeoutCount > 1 ? static_cast<unsigned long>(timeoutCount) : 1UL;
        if (!m_waitCondition.wait(&m_mutex, waitMs)) {
            return false;
        }
    }
    if (m_frames.empty()) {
        return false;
    }
    outFrame = std::move(m_frames.front());
    m_frames.pop_front();
    return true;
}

bool VideoSendFrameRingBuffer::closed() const {
    QMutexLocker locker(&m_mutex);
    return m_closed;
}

}  // namespace av::session
