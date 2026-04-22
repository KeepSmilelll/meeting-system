#include "VideoSessionControlActions.h"

#include <algorithm>

namespace av::session {

void resetSenderForFreshStream(media::RTPSender& sender,
                               VideoRtcpActionPipeline& rtcpActionPipeline,
                               uint32_t ssrc) {
    sender.setSSRC(ssrc);
    sender.setSequence(0);
    rtcpActionPipeline.reset();
}

void resetSendThreadStats(std::atomic<uint64_t>& keyframeRequestCount,
                          std::atomic<uint64_t>& retransmitPacketCount,
                          std::atomic<uint64_t>& bitrateReconfigureCount,
                          std::atomic<uint32_t>& targetBitrateBps,
                          std::atomic<uint32_t>& appliedBitrateBps,
                          std::atomic<uint32_t>& lastBitrateApplyDelayMs,
                          std::atomic<uint64_t>& targetBitrateUpdatedAtMs,
                          uint32_t bitrateBps,
                          uint64_t nowMs) {
    keyframeRequestCount.store(0, std::memory_order_release);
    retransmitPacketCount.store(0, std::memory_order_release);
    bitrateReconfigureCount.store(0, std::memory_order_release);
    targetBitrateBps.store(bitrateBps, std::memory_order_release);
    appliedBitrateBps.store(bitrateBps, std::memory_order_release);
    lastBitrateApplyDelayMs.store(0, std::memory_order_release);
    targetBitrateUpdatedAtMs.store(nowMs, std::memory_order_release);
}

}  // namespace av::session
