#include "BandwidthEstimator.h"

#include <algorithm>

namespace media {

BandwidthEstimator::BandwidthEstimator(Config config)
    : m_config(config) {
    if (m_config.minBitrateKbps == 0) {
        m_config.minBitrateKbps = 1;
    }
    if (m_config.maxBitrateKbps < m_config.minBitrateKbps) {
        m_config.maxBitrateKbps = m_config.minBitrateKbps;
    }
    if (m_config.initialBitrateKbps < m_config.minBitrateKbps || m_config.initialBitrateKbps > m_config.maxBitrateKbps) {
        m_config.initialBitrateKbps = std::max(m_config.minBitrateKbps,
                                               std::min(m_config.maxBitrateKbps, m_config.initialBitrateKbps));
    }
    if (m_config.sampleWindowMs < 100) {
        m_config.sampleWindowMs = 100;
    }

    m_bitrateKbps = m_config.initialBitrateKbps;
}

void BandwidthEstimator::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_windowStart = std::chrono::steady_clock::now();
    m_windowBytes = 0;
    m_bitrateKbps = m_config.initialBitrateKbps;
    m_rttMs = 0;
    m_lossRate = 0.0F;
}

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
    if (m_lossRate > 0.30F || rttMs > 800U) {
        adjusted = static_cast<uint32_t>(adjusted * 0.50F);
    } else if (m_lossRate > 0.15F || rttMs > 500U) {
        adjusted = static_cast<uint32_t>(adjusted * 0.70F);
    } else if (m_lossRate > 0.05F || rttMs > 300U) {
        adjusted = static_cast<uint32_t>(adjusted * 0.85F);
    } else if (m_lossRate < 0.02F && rttMs < 120U) {
        adjusted = static_cast<uint32_t>(adjusted * 1.08F);
    }

    m_bitrateKbps = clampBitrateLocked(adjusted);
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
    if (elapsed < static_cast<int64_t>(m_config.sampleWindowMs)) {
        return;
    }

    if (elapsed > 0) {
        const auto bitsPerSecond = static_cast<uint64_t>(m_windowBytes) * 8ULL * 1000ULL /
                                   static_cast<uint64_t>(elapsed);
        const auto kbps = static_cast<uint32_t>(bitsPerSecond / 1000ULL);
        if (kbps > 0) {
            m_bitrateKbps = clampBitrateLocked(kbps);
        }
    }

    m_windowStart = now;
    m_windowBytes = 0;
}

uint32_t BandwidthEstimator::clampBitrateLocked(uint32_t bitrateKbps) const {
    return std::max(m_config.minBitrateKbps, std::min(m_config.maxBitrateKbps, bitrateKbps));
}

}  // namespace media
