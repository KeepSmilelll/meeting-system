#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace sfu::bwe {

class BandwidthEstimator final {
public:
    struct Sample {
        float packetLoss{0.0F};
        uint32_t rttMs{0};
        uint32_t jitterMs{0};
        uint32_t measuredBitrateKbps{0};
    };

    explicit BandwidthEstimator(uint32_t minBitrateKbps = 150U,
                                uint32_t maxBitrateKbps = 2500U,
                                uint32_t initialBitrateKbps = 800U);

    uint32_t Update(const Sample& sample);
    uint32_t RecommendedBitrateKbps() const;

private:
    uint32_t Clamp(uint32_t bitrateKbps) const;
    uint32_t Downscale(uint32_t bitrateKbps, float factor) const;
    uint32_t Upscale(uint32_t bitrateKbps, float factor) const;

    const uint32_t minBitrateKbps_;
    const uint32_t maxBitrateKbps_;
    mutable std::mutex mutex_;
    uint32_t recommendedBitrateKbps_{0};
};

} // namespace sfu::bwe

