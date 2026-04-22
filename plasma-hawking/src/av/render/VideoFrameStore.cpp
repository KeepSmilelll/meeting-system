#include "VideoFrameStore.h"

#include <QMutexLocker>

#include <memory>
#include <utility>

namespace av::render {

VideoFrameStore::VideoFrameStore(QObject* parent)
    : QObject(parent) {}

void VideoFrameStore::clear() {
    {
        QMutexLocker lock(&m_mutex);
        m_latestFrame.reset();
        ++m_revision;
    }
    emit frameChanged();
}

void VideoFrameStore::setFrame(av::codec::DecodedVideoFrame frame) {
    setFrame(std::make_shared<av::codec::DecodedVideoFrame>(std::move(frame)));
}

void VideoFrameStore::setFrame(FramePtr frame) {
    {
        QMutexLocker lock(&m_mutex);
        m_latestFrame = std::move(frame);
        ++m_revision;
    }
    emit frameChanged();
}

bool VideoFrameStore::snapshot(av::codec::DecodedVideoFrame& outFrame, uint64_t* revision) const {
    const FramePtr frame = snapshotFrame(revision);
    if (!frame) {
        return false;
    }

    outFrame = *frame;
    return true;
}

VideoFrameStore::FramePtr VideoFrameStore::snapshotFrame(uint64_t* revision) const {
    QMutexLocker lock(&m_mutex);
    if (revision != nullptr) {
        *revision = m_revision;
    }

    if (!m_latestFrame || !m_latestFrame->hasRenderableData()) {
        return {};
    }

    return m_latestFrame;
}

}  // namespace av::render
