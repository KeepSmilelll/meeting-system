#include "VideoSendControlActions.h"

#include "av/codec/VideoEncoder.h"

#include <limits>

namespace av::session {
namespace {

constexpr uint32_t kMinAdaptiveBitrateBps = 100000U;

}  // namespace

bool shouldReportCameraFrameTimeout(CameraFrameTimeoutState& state,
                                    uint64_t nowMs,
                                    uint64_t timeoutMs) {
    if (state.waitStartedAtMs == 0) {
        state.waitStartedAtMs = nowMs;
        return false;
    }
    if (state.timeoutReported) {
        return false;
    }
    if (nowMs - state.waitStartedAtMs < timeoutMs) {
        return false;
    }

    state.timeoutReported = true;
    return true;
}

void resetCameraFrameTimeout(CameraFrameTimeoutState& state) {
    state.timeoutReported = false;
    state.waitStartedAtMs = 0;
}

bool maybeApplyTargetBitrate(av::codec::VideoEncoder& encoder,
                             std::atomic<uint32_t>& targetBitrateBps,
                             std::atomic<uint32_t>& appliedBitrateBps,
                             std::atomic<uint64_t>& bitrateReconfigureCount,
                             std::atomic<uint64_t>& targetBitrateUpdatedAtMs,
                             std::atomic<uint32_t>& lastBitrateApplyDelayMs,
                             uint64_t nowMs,
                             std::string* error) {
    const uint32_t targetBitrate = targetBitrateBps.load(std::memory_order_acquire);
    if (targetBitrate < kMinAdaptiveBitrateBps || static_cast<int>(targetBitrate) == encoder.bitrate()) {
        return true;
    }

    if (!encoder.setBitrate(static_cast<int>(targetBitrate))) {
        if (error) {
            *error = "video encoder bitrate update failed";
        }
        return false;
    }

    appliedBitrateBps.store(static_cast<uint32_t>(encoder.bitrate()), std::memory_order_release);
    bitrateReconfigureCount.fetch_add(1, std::memory_order_acq_rel);

    const uint64_t updatedAtMs = targetBitrateUpdatedAtMs.load(std::memory_order_acquire);
    if (updatedAtMs != 0 && nowMs >= updatedAtMs) {
        const uint64_t delayMs = nowMs - updatedAtMs;
        const uint32_t boundedDelay = delayMs > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(delayMs);
        lastBitrateApplyDelayMs.store(boundedDelay, std::memory_order_release);
    }

    return true;
}

}  // namespace av::session
