#pragma once

#include <cstdint>

namespace av::session {

class VideoRecvKeyFramePipeline {
public:
    explicit VideoRecvKeyFramePipeline(uint64_t cooldownMs = 300U);

    bool shouldSendPli(uint32_t mediaSsrc,
                       uint64_t nowMs,
                       uint64_t lastPliRequestedAtMs) const;

    void markPliSent(uint64_t nowMs, uint64_t& lastPliRequestedAtMs) const;

private:
    uint64_t m_cooldownMs{300U};
};

}  // namespace av::session
