#pragma once

#include "av/codec/VideoDecoder.h"

#include <QObject>

#include <cstdint>
#include <mutex>

namespace av::render {

class VideoFrameStore : public QObject {
    Q_OBJECT

public:
    explicit VideoFrameStore(QObject* parent = nullptr);

    void clear();
    void setFrame(av::codec::DecodedVideoFrame frame);
    bool snapshot(av::codec::DecodedVideoFrame& outFrame, uint64_t* revision = nullptr) const;

signals:
    void frameChanged();

private:
    mutable std::mutex m_mutex;
    av::codec::DecodedVideoFrame m_latestFrame;
    uint64_t m_revision{0};
};

}  // namespace av::render
