#include "VideoFrameStore.h"

#include <utility>

namespace av::render {

VideoFrameStore::VideoFrameStore(QObject* parent)
    : QObject(parent) {}

void VideoFrameStore::clear() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latestFrame = {};
        ++m_revision;
    }
    emit frameChanged();
}

void VideoFrameStore::setFrame(av::codec::DecodedVideoFrame frame) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latestFrame = std::move(frame);
        ++m_revision;
    }
    emit frameChanged();
}

bool VideoFrameStore::snapshot(av::codec::DecodedVideoFrame& outFrame, uint64_t* revision) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_latestFrame.width <= 0 || m_latestFrame.height <= 0 ||
        m_latestFrame.yPlane.empty() || m_latestFrame.uvPlane.empty()) {
        return false;
    }

    outFrame = m_latestFrame;
    if (revision != nullptr) {
        *revision = m_revision;
    }
    return true;
}

}  // namespace av::render
