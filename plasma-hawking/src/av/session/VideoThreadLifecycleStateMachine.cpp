#include "VideoThreadLifecycleStateMachine.h"

namespace av::session {

VideoSessionStartPlan VideoThreadLifecycleStateMachine::planSessionStart(bool running) {
    VideoSessionStartPlan plan{};
    plan.alreadyRunning = running;
    plan.shouldStartRecvThread = !running;
    return plan;
}

VideoSessionStopPlan VideoThreadLifecycleStateMachine::planSessionStop(bool wasRunning,
                                                                       bool sendThreadJoinable,
                                                                       bool recvThreadJoinable) {
    VideoSessionStopPlan plan{};
    plan.shouldReturnEarly = !wasRunning && !sendThreadJoinable && !recvThreadJoinable;
    plan.shouldJoinSendBeforeCleanup = sendThreadJoinable;
    plan.shouldJoinRecvAfterCleanup = recvThreadJoinable;
    return plan;
}

VideoSendThreadStartPlan VideoThreadLifecycleStateMachine::planSendThreadStart(bool requestStartSendThread,
                                                                                bool sendThreadJoinable) {
    VideoSendThreadStartPlan plan{};
    plan.shouldStartSendThread = requestStartSendThread;
    plan.shouldJoinExistingSendThread = requestStartSendThread && sendThreadJoinable;
    return plan;
}

VideoSendThreadStopPlan VideoThreadLifecycleStateMachine::planSendThreadStop(bool requestJoinSendThread,
                                                                              bool sendThreadJoinable) {
    VideoSendThreadStopPlan plan{};
    plan.shouldJoinSendThread = requestJoinSendThread && sendThreadJoinable;
    return plan;
}

}  // namespace av::session
