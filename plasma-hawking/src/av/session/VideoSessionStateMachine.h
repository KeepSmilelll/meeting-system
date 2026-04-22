#pragma once

namespace av::session {

enum class VideoSendSource {
    None,
    Screen,
    Camera,
};

struct VideoToggleEnablePlan {
    bool alreadyEnabled{false};
    bool shouldStartSendThread{false};
    bool shouldRequestKeyFrame{false};
};

struct VideoToggleDisablePlan {
    bool alreadyDisabled{false};
    bool shouldJoinSendThread{false};
    bool shouldSetForceKeyFrame{false};
    bool forceKeyFrameValue{false};
};

class VideoSessionStateMachine {
public:
    static VideoToggleEnablePlan planEnableSharing(bool sharingEnabled, bool cameraSendingEnabled);
    static VideoToggleEnablePlan planEnableCamera(bool cameraSendingEnabled, bool sharingEnabled);

    static VideoToggleDisablePlan planDisableSharing(bool sharingEnabled, bool cameraSendingEnabled);
    static VideoToggleDisablePlan planDisableCamera(bool cameraSendingEnabled, bool sharingEnabled);

    static VideoSendSource resolveSendSource(bool sharingEnabled, bool cameraSendingEnabled);
};

}  // namespace av::session
