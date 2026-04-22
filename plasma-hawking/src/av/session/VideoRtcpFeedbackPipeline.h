#pragma once

#include "net/media/RTCPHandler.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace av::session {

enum class VideoRtcpFeedbackEventKind {
    Nack,
    Pli,
    Remb,
};

struct VideoRtcpFeedbackEvent {
    VideoRtcpFeedbackEventKind kind{VideoRtcpFeedbackEventKind::Nack};
    uint16_t sequenceNumber{0};
    uint32_t mediaSsrc{0};
    uint32_t bitrateBps{0};
};

class VideoRtcpFeedbackPipeline {
public:
    bool parseFeedback(const uint8_t* data,
                       std::size_t len,
                       uint32_t localSsrc,
                       std::vector<VideoRtcpFeedbackEvent>& outEvents) const;

private:
    media::RTCPHandler m_rtcpHandler;
};

}  // namespace av::session
