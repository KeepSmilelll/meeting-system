#include "VideoRecvKeyFramePipeline.h"

namespace av::session {

VideoRecvKeyFramePipeline::VideoRecvKeyFramePipeline(uint64_t cooldownMs)
    : m_cooldownMs(cooldownMs) {}

bool VideoRecvKeyFramePipeline::shouldSendPli(uint32_t mediaSsrc,
                                              uint64_t nowMs) const {
    if (mediaSsrc == 0U) {
        return false;
    }
    const auto found = m_lastPliRequestedAtMsBySsrc.find(mediaSsrc);
    if (found == m_lastPliRequestedAtMsBySsrc.end()) {
        return true;
    }
    const uint64_t lastPliRequestedAtMs = found->second;
    if (lastPliRequestedAtMs != 0U && nowMs - lastPliRequestedAtMs < m_cooldownMs) {
        return false;
    }
    return true;
}

void VideoRecvKeyFramePipeline::markPliSent(uint32_t mediaSsrc, uint64_t nowMs) {
    if (mediaSsrc == 0U) {
        return;
    }
    m_lastPliRequestedAtMsBySsrc[mediaSsrc] = nowMs;
}

void VideoRecvKeyFramePipeline::reset(uint32_t mediaSsrc) {
    if (mediaSsrc == 0U) {
        m_lastPliRequestedAtMsBySsrc.clear();
        return;
    }
    m_lastPliRequestedAtMsBySsrc.erase(mediaSsrc);
}

}  // namespace av::session
