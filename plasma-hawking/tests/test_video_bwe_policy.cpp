#include "av/session/VideoBwePolicy.h"

#include <cassert>
#include <cstdint>

namespace {

uint8_t fractionForLossPercent(double percent) {
    return static_cast<uint8_t>((percent * 256.0) / 100.0 + 0.5);
}

}  // namespace

int main() {
    av::session::VideoBwePolicy policy(av::session::VideoBwePolicyConfig{
        1280,
        720,
        30,
        1000U * 1000U,
    });

    assert(policy.target().width == 1280);
    assert(policy.target().height == 720);
    assert(policy.target().frameRate == 30);
    assert(policy.target().bitrateBps == 1000U * 1000U);

    av::session::VideoBwePolicySample sample{};
    sample.nowMs = 1000U;
    sample.fractionLost = fractionForLossPercent(6.0);
    assert(policy.onReceiverReport(sample));
    assert(policy.target().level == av::session::VideoBweDegradationLevel::Moderate);
    assert(policy.target().width == 1280);
    assert(policy.target().height == 720);
    assert(policy.target().frameRate == 15);
    assert(policy.target().bitrateBps == 700U * 1000U);
    assert(!policy.target().videoSuspended);

    sample.nowMs = 2000U;
    sample.fractionLost = fractionForLossPercent(16.0);
    assert(policy.onReceiverReport(sample));
    assert(policy.target().level == av::session::VideoBweDegradationLevel::Severe);
    assert(policy.target().width == 640);
    assert(policy.target().height == 360);
    assert(policy.target().frameRate == 15);
    assert(policy.target().bitrateBps == 300U * 1000U);

    sample.nowMs = 3000U;
    sample.fractionLost = fractionForLossPercent(31.0);
    assert(policy.onReceiverReport(sample));
    assert(policy.target().level == av::session::VideoBweDegradationLevel::AudioOnly);
    assert(policy.target().videoSuspended);
    assert(policy.target().bitrateBps == 0U);

    sample.nowMs = 8000U;
    sample.fractionLost = fractionForLossPercent(1.0);
    assert(policy.onReceiverReport(sample));
    assert(!policy.target().videoSuspended);
    assert(policy.target().bitrateBps == 330U * 1000U);
    assert(policy.target().frameRate == 15);

    sample.nowMs = 9000U;
    sample.hasRtt = true;
    sample.rttMs = 350U;
    sample.fractionLost = fractionForLossPercent(1.0);
    assert(policy.onReceiverReport(sample));
    assert(policy.target().jitterTargetMs == 100U);
    assert(!policy.target().requestTurnRelay);

    sample.nowMs = 10000U;
    sample.rttMs = 550U;
    assert(policy.onReceiverReport(sample));
    assert(policy.target().requestTurnRelay);
    assert(policy.target().jitterTargetMs == 100U);

    sample.nowMs = 11000U;
    assert(policy.onReceiverReport(sample));
    assert(!policy.target().requestTurnRelay);

    assert(policy.onRembTarget(250U * 1000U, 12000U));
    assert(policy.target().bitrateBps == 250U * 1000U);

    sample.nowMs = 18000U;
    sample.hasRtt = false;
    sample.fractionLost = fractionForLossPercent(1.0);
    assert(policy.onReceiverReport(sample));
    assert(policy.target().bitrateBps == 250U * 1000U);

    return 0;
}
