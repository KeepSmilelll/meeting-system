#pragma once

#include "VideoSendPipeline.h"

#include <functional>
#include <string>

namespace av::session {

struct VideoSendTelemetryState {
    bool loggedFirstPacket{false};
    bool loggedFirstEncodedPacket{false};
    bool loggedFirstEncodePending{false};
    bool loggedFirstEncodeError{false};
};

class VideoSendTelemetryPipeline {
public:
    void onEncodePending(VideoSendTelemetryState& state,
                         const std::function<void(std::string)>& statusCallback) const;

    void onEncodeError(VideoSendTelemetryState& state,
                       const std::string& pipelineError,
                       const std::function<void(std::string)>& statusCallback) const;

    void onEncodedPacketObserved(VideoSendTelemetryState& state,
                                 const std::function<void(std::string)>& statusCallback) const;

    void onPacketSent(VideoSendTelemetryState& state,
                      const VideoSendPipelinePacket& packet,
                      const std::function<void(std::string)>& statusCallback) const;
};

}  // namespace av::session
