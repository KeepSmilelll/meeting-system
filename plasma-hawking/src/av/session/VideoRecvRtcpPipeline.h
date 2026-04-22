#pragma once

#include "VideoRtcpFeedbackDispatchPipeline.h"
#include "VideoRtcpFeedbackPipeline.h"

#include <cstddef>
#include <cstdint>

namespace av::session {

class VideoRecvRtcpPipeline {
public:
    bool parseDispatchPlan(const uint8_t* data,
                           std::size_t len,
                           uint32_t localSsrc,
                           const VideoRtcpFeedbackPipeline& feedbackPipeline,
                           const VideoRtcpFeedbackDispatchPipeline& dispatchPipeline,
                           VideoRtcpFeedbackDispatchPlan& outDispatchPlan) const;
};

}  // namespace av::session
