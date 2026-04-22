#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace av::codec {
class VideoEncoder;
}

namespace av::session {

struct CameraFrameTimeoutState {
    bool timeoutReported{false};
    uint64_t waitStartedAtMs{0};
};

bool shouldReportCameraFrameTimeout(CameraFrameTimeoutState& state,
                                    uint64_t nowMs,
                                    uint64_t timeoutMs);

void resetCameraFrameTimeout(CameraFrameTimeoutState& state);

bool maybeApplyTargetBitrate(av::codec::VideoEncoder& encoder,
                             std::atomic<uint32_t>& targetBitrateBps,
                             std::atomic<uint32_t>& appliedBitrateBps,
                             std::atomic<uint64_t>& bitrateReconfigureCount,
                             std::atomic<uint64_t>& targetBitrateUpdatedAtMs,
                             std::atomic<uint32_t>& lastBitrateApplyDelayMs,
                             uint64_t nowMs,
                             std::string* error);

}  // namespace av::session
