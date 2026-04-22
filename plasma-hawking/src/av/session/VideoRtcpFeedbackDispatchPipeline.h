#pragma once

#include "VideoRtcpFeedbackPipeline.h"

#include <cstdint>
#include <vector>

namespace av::session {

struct VideoRtcpFeedbackDispatchPlan {
    std::vector<uint16_t> retransmitSequenceNumbers;
    bool requestKeyFrame{false};
    bool hasTargetBitrate{false};
    uint32_t targetBitrateBps{0};

    bool hasActions() const;
};

class VideoRtcpFeedbackDispatchPipeline {
public:
    VideoRtcpFeedbackDispatchPlan buildPlan(const std::vector<VideoRtcpFeedbackEvent>& events) const;
};

}  // namespace av::session
