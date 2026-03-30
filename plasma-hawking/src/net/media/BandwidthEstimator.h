#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace media {

class BandwidthEstimator {
public:
    void onPacketSent(std::size_t bytes);
    void onReceiverReport(uint8_t fractionLost, uint32_t rttMs);

    uint32_t estimateBitrateKbps() const;
    uint32_t rttMs() const;
    float lossRate() const;

private:
    void refreshWindowLocked(std::chrono::steady_clock::time_point now) const;

    mutable std::mutex m_mutex;
    mutable std::chrono::steady_clock::time_point m_windowStart{std::chrono::steady_clock::now()};
    mutable std::size_t m_windowBytes{0};
    mutable uint32_t m_bitrateKbps{300};
    mutable uint32_t m_rttMs{0};
    mutable float m_lossRate{0.0F};
};

}  // namespace media
