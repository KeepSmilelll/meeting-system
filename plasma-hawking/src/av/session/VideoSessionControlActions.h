#pragma once

#include "VideoRtcpActionPipeline.h"
#include "net/media/RTPSender.h"

#include <atomic>
#include <cstdint>

namespace av::session {

void resetSenderForFreshStream(media::RTPSender& sender,
                               VideoRtcpActionPipeline& rtcpActionPipeline,
                               uint32_t ssrc);

void resetSendThreadStats(std::atomic<uint64_t>& keyframeRequestCount,
                          std::atomic<uint64_t>& retransmitPacketCount,
                          std::atomic<uint64_t>& bitrateReconfigureCount,
                          std::atomic<uint32_t>& targetBitrateBps,
                          std::atomic<uint32_t>& appliedBitrateBps,
                          std::atomic<uint32_t>& lastBitrateApplyDelayMs,
                          std::atomic<uint64_t>& targetBitrateUpdatedAtMs,
                          uint32_t bitrateBps,
                          uint64_t nowMs);

}  // namespace av::session
