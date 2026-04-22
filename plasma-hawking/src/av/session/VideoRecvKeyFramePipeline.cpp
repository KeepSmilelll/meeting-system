#include "VideoRecvKeyFramePipeline.h"

namespace av::session {

VideoRecvKeyFramePipeline::VideoRecvKeyFramePipeline(uint64_t cooldownMs)
    : m_cooldownMs(cooldownMs) {}

bool VideoRecvKeyFramePipeline::shouldSendPli(uint32_t mediaSsrc,
                                              uint64_t nowMs,
                                              uint64_t lastPliRequestedAtMs) const {
    if (mediaSsrc == 0U) {
        return false;
    }
    if (lastPliRequestedAtMs != 0U && nowMs - lastPliRequestedAtMs < m_cooldownMs) {
        return false;
    }
    return true;
}

void VideoRecvKeyFramePipeline::markPliSent(uint64_t nowMs, uint64_t& lastPliRequestedAtMs) const {
    lastPliRequestedAtMs = nowMs;
}

}  // namespace av::session
