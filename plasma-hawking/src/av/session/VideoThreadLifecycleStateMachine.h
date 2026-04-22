#pragma once

namespace av::session {

struct VideoSessionStartPlan {
    bool alreadyRunning{false};
    bool shouldStartRecvThread{false};
};

struct VideoSessionStopPlan {
    bool shouldReturnEarly{false};
    bool shouldJoinSendBeforeCleanup{false};
    bool shouldJoinRecvAfterCleanup{false};
};

struct VideoSendThreadStartPlan {
    bool shouldStartSendThread{false};
    bool shouldJoinExistingSendThread{false};
};

struct VideoSendThreadStopPlan {
    bool shouldJoinSendThread{false};
};

class VideoThreadLifecycleStateMachine {
public:
    static VideoSessionStartPlan planSessionStart(bool running);
    static VideoSessionStopPlan planSessionStop(bool wasRunning,
                                                bool sendThreadJoinable,
                                                bool recvThreadJoinable);

    static VideoSendThreadStartPlan planSendThreadStart(bool requestStartSendThread,
                                                        bool sendThreadJoinable);
    static VideoSendThreadStopPlan planSendThreadStop(bool requestJoinSendThread,
                                                      bool sendThreadJoinable);
};

}  // namespace av::session
