#include "VideoBwePolicy.h"

#include <algorithm>
#include <cmath>

namespace av::session {
namespace {

constexpr uint32_t kMinVideoBitrateBps = 100000U;
constexpr uint32_t kSevereVideoBitrateBps = 300000U;
constexpr int kDegradedFrameRate = 15;
constexpr uint32_t kHighRttJitterTargetMs = 100U;
constexpr uint64_t kRestoreIntervalMs = 5000U;

int evenAtLeastTwo(int value) {
    value = std::max(2, value);
    value &= ~1;
    return std::max(2, value);
}

VideoBwePolicyConfig normalizeConfig(VideoBwePolicyConfig config) {
    config.baselineWidth = evenAtLeastTwo(config.baselineWidth);
    config.baselineHeight = evenAtLeastTwo(config.baselineHeight);
    config.baselineFrameRate = std::max(1, config.baselineFrameRate);
    config.baselineBitrateBps = std::max(kMinVideoBitrateBps, config.baselineBitrateBps);
    return config;
}

double lossPercent(uint8_t fractionLost) {
    return (static_cast<double>(fractionLost) * 100.0) / 256.0;
}

int scaledWidthFor360p(const VideoBwePolicyConfig& config) {
    if (config.baselineHeight <= 360) {
        return config.baselineWidth;
    }
    const double ratio = 360.0 / static_cast<double>(config.baselineHeight);
    return evenAtLeastTwo(static_cast<int>(std::round(static_cast<double>(config.baselineWidth) * ratio)));
}

int scaledHeightFor360p(const VideoBwePolicyConfig& config) {
    return config.baselineHeight <= 360 ? config.baselineHeight : 360;
}

uint32_t reduceByThirtyPercent(uint32_t bitrateBps) {
    const uint64_t reduced = (static_cast<uint64_t>(bitrateBps) * 70U) / 100U;
    return static_cast<uint32_t>(std::max<uint64_t>(kMinVideoBitrateBps, reduced));
}

uint32_t restoreByTenPercent(uint32_t bitrateBps, uint32_t baselineBitrateBps) {
    const uint64_t restored = (static_cast<uint64_t>(bitrateBps) * 110U + 99U) / 100U;
    return static_cast<uint32_t>(std::min<uint64_t>(baselineBitrateBps, restored));
}

}  // namespace

VideoBwePolicy::VideoBwePolicy(VideoBwePolicyConfig config) {
    reset(config, 0U);
}

const VideoBwePolicyTarget& VideoBwePolicy::target() const {
    return m_target;
}

void VideoBwePolicy::reset(VideoBwePolicyConfig config, uint64_t nowMs) {
    m_config = normalizeConfig(config);
    m_policyBitrateBps = m_config.baselineBitrateBps;
    m_rembLimitBps = 0U;
    m_lastRestoreAtMs = nowMs;
    m_turnRelayRequested = false;
    m_target = VideoBwePolicyTarget{};
    publishTarget(VideoBweDegradationLevel::Normal,
                  m_config.baselineWidth,
                  m_config.baselineHeight,
                  m_config.baselineFrameRate,
                  m_policyBitrateBps,
                  false,
                  0U,
                  false);
}

bool VideoBwePolicy::onReceiverReport(const VideoBwePolicySample& sample) {
    const VideoBwePolicyTarget previous = m_target;
    const double loss = lossPercent(sample.fractionLost);
    const bool highRtt = sample.hasRtt && sample.rttMs > 300U;
    const bool turnRtt = sample.hasRtt && sample.rttMs > 500U;
    const uint32_t jitterTargetMs = highRtt ? kHighRttJitterTargetMs : 0U;
    const bool requestTurnRelay = turnRtt && !m_turnRelayRequested;
    if (requestTurnRelay) {
        m_turnRelayRequested = true;
    }

    if (loss > 30.0) {
        publishTarget(VideoBweDegradationLevel::AudioOnly,
                      m_target.width,
                      m_target.height,
                      kDegradedFrameRate,
                      0U,
                      true,
                      jitterTargetMs,
                      requestTurnRelay);
    } else if (loss > 15.0) {
        m_policyBitrateBps = std::min(m_policyBitrateBps, kSevereVideoBitrateBps);
        publishTarget(VideoBweDegradationLevel::Severe,
                      scaledWidthFor360p(m_config),
                      scaledHeightFor360p(m_config),
                      kDegradedFrameRate,
                      m_policyBitrateBps,
                      false,
                      jitterTargetMs,
                      requestTurnRelay);
    } else if (loss > 5.0) {
        m_policyBitrateBps = reduceByThirtyPercent(
            m_policyBitrateBps == 0U ? m_config.baselineBitrateBps : m_policyBitrateBps);
        publishTarget(VideoBweDegradationLevel::Moderate,
                      m_config.baselineWidth,
                      m_config.baselineHeight,
                      kDegradedFrameRate,
                      m_policyBitrateBps,
                      false,
                      jitterTargetMs,
                      requestTurnRelay);
    } else if (loss < 2.0) {
        if (sample.nowMs >= m_lastRestoreAtMs &&
            sample.nowMs - m_lastRestoreAtMs >= kRestoreIntervalMs) {
            m_lastRestoreAtMs = sample.nowMs;
            if (m_policyBitrateBps == 0U) {
                m_policyBitrateBps = kSevereVideoBitrateBps;
            } else {
                m_policyBitrateBps = restoreByTenPercent(m_policyBitrateBps, m_config.baselineBitrateBps);
            }
        }

        const bool fullyRestored = m_policyBitrateBps >= m_config.baselineBitrateBps;
        publishTarget(fullyRestored ? VideoBweDegradationLevel::Normal : VideoBweDegradationLevel::Moderate,
                      fullyRestored ? m_config.baselineWidth : m_target.width,
                      fullyRestored ? m_config.baselineHeight : m_target.height,
                      fullyRestored ? m_config.baselineFrameRate : kDegradedFrameRate,
                      m_policyBitrateBps,
                      false,
                      jitterTargetMs,
                      requestTurnRelay);
    } else if (m_target.jitterTargetMs != jitterTargetMs || requestTurnRelay) {
        publishTarget(m_target.level,
                      m_target.width,
                      m_target.height,
                      m_target.frameRate,
                      m_policyBitrateBps,
                      m_target.videoSuspended,
                      jitterTargetMs,
                      requestTurnRelay);
    }

    return previous.version != m_target.version || requestTurnRelay;
}

bool VideoBwePolicy::onRembTarget(uint32_t bitrateBps, uint64_t nowMs) {
    (void)nowMs;
    const VideoBwePolicyTarget previous = m_target;
    m_rembLimitBps = std::max(kMinVideoBitrateBps, bitrateBps);
    publishTarget(m_target.level,
                  m_target.width,
                  m_target.height,
                  m_target.frameRate,
                  m_policyBitrateBps,
                  m_target.videoSuspended,
                  m_target.jitterTargetMs,
                  false);
    return previous.version != m_target.version;
}

void VideoBwePolicy::publishTarget(VideoBweDegradationLevel level,
                                   int width,
                                   int height,
                                   int frameRate,
                                   uint32_t policyBitrateBps,
                                   bool videoSuspended,
                                   uint32_t jitterTargetMs,
                                   bool requestTurnRelay) {
    width = evenAtLeastTwo(width);
    height = evenAtLeastTwo(height);
    frameRate = std::max(1, frameRate);
    const uint32_t bitrateBps = videoSuspended ? 0U : effectiveBitrate(policyBitrateBps);

    if (m_target.level == level &&
        m_target.width == width &&
        m_target.height == height &&
        m_target.frameRate == frameRate &&
        m_target.bitrateBps == bitrateBps &&
        m_target.videoSuspended == videoSuspended &&
        m_target.jitterTargetMs == jitterTargetMs &&
        m_target.requestTurnRelay == requestTurnRelay) {
        return;
    }

    m_target.level = level;
    m_target.width = width;
    m_target.height = height;
    m_target.frameRate = frameRate;
    m_target.bitrateBps = bitrateBps;
    m_target.videoSuspended = videoSuspended;
    m_target.jitterTargetMs = jitterTargetMs;
    m_target.requestTurnRelay = requestTurnRelay;
    ++m_target.version;
}

uint32_t VideoBwePolicy::effectiveBitrate(uint32_t policyBitrateBps) const {
    uint32_t bitrate = std::clamp(policyBitrateBps, kMinVideoBitrateBps, m_config.baselineBitrateBps);
    if (m_rembLimitBps >= kMinVideoBitrateBps) {
        bitrate = std::min(bitrate, m_rembLimitBps);
    }
    return bitrate;
}

}  // namespace av::session
