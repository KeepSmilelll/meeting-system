#include "VideoSessionStateMachine.h"

namespace av::session {

VideoToggleEnablePlan VideoSessionStateMachine::planEnableSharing(bool sharingEnabled,
                                                                  bool cameraSendingEnabled) {
    VideoToggleEnablePlan plan{};
    plan.alreadyEnabled = sharingEnabled;
    if (plan.alreadyEnabled) {
        return plan;
    }

    plan.shouldStartSendThread = !cameraSendingEnabled;
    plan.shouldRequestKeyFrame = true;
    return plan;
}

VideoToggleEnablePlan VideoSessionStateMachine::planEnableCamera(bool cameraSendingEnabled,
                                                                 bool sharingEnabled) {
    VideoToggleEnablePlan plan{};
    plan.alreadyEnabled = cameraSendingEnabled;
    if (plan.alreadyEnabled) {
        return plan;
    }

    plan.shouldStartSendThread = !sharingEnabled;
    plan.shouldRequestKeyFrame = !sharingEnabled;
    return plan;
}

VideoToggleDisablePlan VideoSessionStateMachine::planDisableSharing(bool sharingEnabled,
                                                                    bool cameraSendingEnabled) {
    VideoToggleDisablePlan plan{};
    plan.alreadyDisabled = !sharingEnabled;
    if (plan.alreadyDisabled) {
        return plan;
    }

    plan.shouldJoinSendThread = !cameraSendingEnabled;
    plan.shouldSetForceKeyFrame = true;
    plan.forceKeyFrameValue = cameraSendingEnabled;
    return plan;
}

VideoToggleDisablePlan VideoSessionStateMachine::planDisableCamera(bool cameraSendingEnabled,
                                                                   bool sharingEnabled) {
    VideoToggleDisablePlan plan{};
    plan.alreadyDisabled = !cameraSendingEnabled;
    if (plan.alreadyDisabled) {
        return plan;
    }

    plan.shouldJoinSendThread = !sharingEnabled;
    plan.shouldSetForceKeyFrame = !sharingEnabled;
    plan.forceKeyFrameValue = false;
    return plan;
}

VideoSendSource VideoSessionStateMachine::resolveSendSource(bool sharingEnabled,
                                                            bool cameraSendingEnabled) {
    if (sharingEnabled) {
        return VideoSendSource::Screen;
    }
    if (cameraSendingEnabled) {
        return VideoSendSource::Camera;
    }
    return VideoSendSource::None;
}

}  // namespace av::session
