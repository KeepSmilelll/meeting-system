#include "bwe/BandwidthEstimator.h"

namespace sfu::bwe {

BandwidthEstimator::BandwidthEstimator(uint32_t minBitrateKbps,
                                       uint32_t maxBitrateKbps,
                                       uint32_t initialBitrateKbps)
    : minBitrateKbps_(std::max<uint32_t>(1U, minBitrateKbps))
    , maxBitrateKbps_(std::max(minBitrateKbps_, maxBitrateKbps))
    , recommendedBitrateKbps_(Clamp(initialBitrateKbps)) {}

uint32_t BandwidthEstimator::Update(const Sample& sample) {
    std::lock_guard<std::mutex> lock(mutex_);

    const uint32_t measured = sample.measuredBitrateKbps == 0
        ? recommendedBitrateKbps_
        : Clamp(sample.measuredBitrateKbps);
    uint32_t candidate = std::min(recommendedBitrateKbps_, measured);

    if (sample.packetLoss > 0.30F || sample.rttMs > 800U) {
        candidate = Downscale(candidate, 0.50F);
    } else if (sample.packetLoss > 0.15F || sample.rttMs > 500U) {
        candidate = Downscale(candidate, 0.70F);
    } else if (sample.packetLoss > 0.05F || sample.rttMs > 300U || sample.jitterMs > 120U) {
        candidate = Downscale(candidate, 0.85F);
    } else if (sample.packetLoss < 0.02F && sample.rttMs < 150U && sample.jitterMs < 60U) {
        candidate = Upscale(candidate, 1.10F);
    } else {
        candidate = static_cast<uint32_t>((static_cast<uint64_t>(candidate) * 3ULL +
                                           static_cast<uint64_t>(measured)) / 4ULL);
    }

    recommendedBitrateKbps_ = Clamp(candidate);
    return recommendedBitrateKbps_;
}

uint32_t BandwidthEstimator::RecommendedBitrateKbps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recommendedBitrateKbps_;
}

uint32_t BandwidthEstimator::Clamp(uint32_t bitrateKbps) const {
    return std::max(minBitrateKbps_, std::min(maxBitrateKbps_, bitrateKbps));
}

uint32_t BandwidthEstimator::Downscale(uint32_t bitrateKbps, float factor) const {
    if (!(factor > 0.0F && factor < 1.0F)) {
        return Clamp(bitrateKbps);
    }
    return Clamp(static_cast<uint32_t>(static_cast<float>(bitrateKbps) * factor));
}

uint32_t BandwidthEstimator::Upscale(uint32_t bitrateKbps, float factor) const {
    if (factor <= 1.0F) {
        return Clamp(bitrateKbps);
    }
    return Clamp(static_cast<uint32_t>(static_cast<float>(bitrateKbps) * factor));
}

} // namespace sfu::bwe

