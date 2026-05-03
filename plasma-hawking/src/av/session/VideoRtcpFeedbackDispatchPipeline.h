#pragma once

#include "VideoRtcpFeedbackPipeline.h"

#include <cstdint>
#include <vector>

namespace av::session {

struct VideoRtcpReceiverReportSample {
    uint8_t fractionLost{0};
    uint32_t lastSenderReport{0};
    uint32_t delaySinceLastSenderReport{0};
};

struct VideoRtcpFeedbackDispatchPlan {
    std::vector<uint16_t> retransmitSequenceNumbers;
    std::vector<VideoRtcpReceiverReportSample> receiverReports;
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
