#pragma once

#include "av/codec/VideoDecoder.h"

#include <QMutex>
#include <QObject>

#include <cstdint>
#include <memory>

namespace av::render {

class VideoFrameStore : public QObject {
    Q_OBJECT

public:
    using FramePtr = std::shared_ptr<const av::codec::DecodedVideoFrame>;

    explicit VideoFrameStore(QObject* parent = nullptr);

    void clear();
    void setFrame(av::codec::DecodedVideoFrame frame);
    void setFrame(FramePtr frame);
    bool snapshot(av::codec::DecodedVideoFrame& outFrame, uint64_t* revision = nullptr) const;
    FramePtr snapshotFrame(uint64_t* revision = nullptr) const;

signals:
    void frameChanged();

private:
    mutable QMutex m_mutex;
    FramePtr m_latestFrame;
    uint64_t m_revision{0};
};

}  // namespace av::render
