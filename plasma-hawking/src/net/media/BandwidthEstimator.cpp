#include "BandwidthEstimator.h"

#include <algorithm>

namespace media {

void BandwidthEstimator::onPacketSent(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    refreshWindowLocked(std::chrono::steady_clock::now());
    m_windowBytes += bytes;
}

void BandwidthEstimator::onReceiverReport(uint8_t fractionLost, uint32_t rttMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lossRate = static_cast<float>(fractionLost) / 256.0F;
    m_rttMs = rttMs;

    uint32_t adjusted = m_bitrateKbps;
    if (m_lossRate > 0.10F) {
        adjusted = static_cast<uint32_t>(adjusted * 0.80F);
    } else if (m_lossRate < 0.02F && rttMs < 120) {
        adjusted = static_cast<uint32_t>(adjusted * 1.08F);
    }

    adjusted = std::max<uint32_t>(64, std::min<uint32_t>(2500, adjusted));
    m_bitrateKbps = adjusted;
}

uint32_t BandwidthEstimator::estimateBitrateKbps() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    refreshWindowLocked(std::chrono::steady_clock::now());
    return m_bitrateKbps;
}

uint32_t BandwidthEstimator::rttMs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rttMs;
}

float BandwidthEstimator::lossRate() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lossRate;
}

void BandwidthEstimator::refreshWindowLocked(std::chrono::steady_clock::time_point now) const {
    using namespace std::chrono;
    const auto elapsed = duration_cast<milliseconds>(now - m_windowStart).count();
    if (elapsed < 500) {
        return;
    }

    if (elapsed > 0) {
        const auto bitsPerSecond = static_cast<uint64_t>(m_windowBytes) * 8ULL * 1000ULL /
                                   static_cast<uint64_t>(elapsed);
        const auto kbps = static_cast<uint32_t>(bitsPerSecond / 1000ULL);
        if (kbps > 0) {
            m_bitrateKbps = std::max<uint32_t>(64, std::min<uint32_t>(2500, kbps));
        }
    }

    m_windowStart = now;
    m_windowBytes = 0;
}

}  // namespace media
