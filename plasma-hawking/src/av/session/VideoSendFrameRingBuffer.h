#pragma once

#include "VideoSendPipeline.h"
#include "VideoSessionStateMachine.h"

#include <chrono>
#include <cstddef>
#include <deque>

#include <QMutex>
#include <QWaitCondition>

namespace av::session {

struct VideoSendCapturedFrame {
    VideoSendSource source{VideoSendSource::None};
    VideoSendPipelineInputFrame inputFrame;
};

class VideoSendFrameRingBuffer {
public:
    explicit VideoSendFrameRingBuffer(std::size_t capacity = 4U);

    void reset();
    void close();
    void clear();

    bool push(VideoSendCapturedFrame frame);
    bool popWait(VideoSendCapturedFrame& outFrame, std::chrono::milliseconds timeout);
    bool closed() const;

private:
    const std::size_t m_capacity;
    mutable QMutex m_mutex;
    QWaitCondition m_waitCondition;
    std::deque<VideoSendCapturedFrame> m_frames;
    bool m_closed{false};
};

}  // namespace av::session
