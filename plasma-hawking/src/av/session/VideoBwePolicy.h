#pragma once

#include <cstdint>

namespace av::session {

enum class VideoBweDegradationLevel {
    Normal,
    Moderate,
    Severe,
    AudioOnly,
};

struct VideoBwePolicyConfig {
    int baselineWidth{1280};
    int baselineHeight{720};
    int baselineFrameRate{5};
    uint32_t baselineBitrateBps{1500U * 1000U};
};

struct VideoBwePolicySample {
    uint8_t fractionLost{0};
    bool hasRtt{false};
    uint32_t rttMs{0};
    uint64_t nowMs{0};
};

struct VideoBwePolicyTarget {
    VideoBweDegradationLevel level{VideoBweDegradationLevel::Normal};
    int width{1280};
    int height{720};
    int frameRate{5};
    uint32_t bitrateBps{1500U * 1000U};
    bool videoSuspended{false};
    uint32_t jitterTargetMs{0};
    bool requestTurnRelay{false};
    uint64_t version{0};
};

class VideoBwePolicy {
public:
    explicit VideoBwePolicy(VideoBwePolicyConfig config = {});

    const VideoBwePolicyTarget& target() const;
    void reset(VideoBwePolicyConfig config, uint64_t nowMs);
    bool onReceiverReport(const VideoBwePolicySample& sample);
    bool onRembTarget(uint32_t bitrateBps, uint64_t nowMs);

private:
    void publishTarget(VideoBweDegradationLevel level,
                       int width,
                       int height,
                       int frameRate,
                       uint32_t policyBitrateBps,
                       bool videoSuspended,
                       uint32_t jitterTargetMs,
                       bool requestTurnRelay);
    uint32_t effectiveBitrate(uint32_t policyBitrateBps) const;

    VideoBwePolicyConfig m_config;
    VideoBwePolicyTarget m_target;
    uint32_t m_policyBitrateBps{0};
    uint32_t m_rembLimitBps{0};
    uint64_t m_lastRestoreAtMs{0};
    bool m_turnRelayRequested{false};
};

}  // namespace av::session
