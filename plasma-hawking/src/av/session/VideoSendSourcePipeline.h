#pragma once

#include "CameraFrameRelay.h"
#include "VideoSendPipeline.h"
#include "VideoSendControlActions.h"
#include "VideoSessionStateMachine.h"

#include "av/capture/ScreenCapture.h"

#include <chrono>
#include <memory>
#include <string>

namespace av::session {

enum class VideoSendFrameFetchResult {
    Ready,
    Retry,
    Abort,
};

struct VideoSendSourceSnapshot {
    VideoSendSource source{VideoSendSource::None};
    std::shared_ptr<av::capture::ScreenCapture> screenCapture;
    std::shared_ptr<av::capture::ScreenCapture> cameraFallbackCapture;
    std::shared_ptr<CameraFrameRelay> cameraRelay;
    bool cameraCaptureRunning{false};
    std::string preferredCameraName;
    bool sharingEnabled{false};
    bool cameraSendingEnabled{false};
};

class VideoSendSourcePipeline {
public:
    VideoSendFrameFetchResult pullFrame(const VideoSendSourceSnapshot& snapshot,
                                        std::chrono::milliseconds waitTimeout,
                                        CameraFrameTimeoutState& timeoutState,
                                        VideoSendPipelineInputFrame& outFrame,
                                        std::string* statusMessage = nullptr) const;
};

}  // namespace av::session
