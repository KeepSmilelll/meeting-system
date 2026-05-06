#pragma once

#include <cstdint>
#include <unordered_map>

namespace av::session {

class VideoRecvKeyFramePipeline {
public:
    explicit VideoRecvKeyFramePipeline(uint64_t cooldownMs = 1500U);

    bool shouldSendPli(uint32_t mediaSsrc,
                       uint64_t nowMs) const;

    void markPliSent(uint32_t mediaSsrc, uint64_t nowMs);
    void reset(uint32_t mediaSsrc);

private:
    uint64_t m_cooldownMs{1500U};
    std::unordered_map<uint32_t, uint64_t> m_lastPliRequestedAtMsBySsrc;
};

}  // namespace av::session
