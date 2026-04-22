#include "VideoRecvRtcpPipeline.h"

#include <vector>

namespace av::session {

bool VideoRecvRtcpPipeline::parseDispatchPlan(
    const uint8_t* data,
    std::size_t len,
    uint32_t localSsrc,
    const VideoRtcpFeedbackPipeline& feedbackPipeline,
    const VideoRtcpFeedbackDispatchPipeline& dispatchPipeline,
    VideoRtcpFeedbackDispatchPlan& outDispatchPlan) const {
    outDispatchPlan = VideoRtcpFeedbackDispatchPlan{};

    std::vector<VideoRtcpFeedbackEvent> events;
    if (!feedbackPipeline.parseFeedback(data, len, localSsrc, events)) {
        return false;
    }

    outDispatchPlan = dispatchPipeline.buildPlan(events);
    return outDispatchPlan.hasActions();
}

}  // namespace av::session
