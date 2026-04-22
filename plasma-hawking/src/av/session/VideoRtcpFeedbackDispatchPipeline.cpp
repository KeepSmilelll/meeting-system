#include "VideoRtcpFeedbackDispatchPipeline.h"

#include <algorithm>

namespace av::session {
namespace {

constexpr uint32_t kMinAdaptiveBitrateBps = 100000U;
constexpr uint32_t kMaxAdaptiveBitrateBps = 8000000U;

}  // namespace

bool VideoRtcpFeedbackDispatchPlan::hasActions() const {
    return requestKeyFrame || hasTargetBitrate || !retransmitSequenceNumbers.empty();
}

VideoRtcpFeedbackDispatchPlan VideoRtcpFeedbackDispatchPipeline::buildPlan(
    const std::vector<VideoRtcpFeedbackEvent>& events) const {
    VideoRtcpFeedbackDispatchPlan plan{};
    for (const auto& event : events) {
        switch (event.kind) {
        case VideoRtcpFeedbackEventKind::Nack:
            plan.retransmitSequenceNumbers.push_back(event.sequenceNumber);
            break;
        case VideoRtcpFeedbackEventKind::Pli:
            plan.requestKeyFrame = true;
            break;
        case VideoRtcpFeedbackEventKind::Remb:
            plan.hasTargetBitrate = true;
            plan.targetBitrateBps =
                std::clamp(event.bitrateBps, kMinAdaptiveBitrateBps, kMaxAdaptiveBitrateBps);
            break;
        default:
            break;
        }
    }
    return plan;
}

}  // namespace av::session
