#pragma once

#include "VideoSendSourcePipeline.h"

#include "av/capture/ScreenCapture.h"

#include <memory>
#include <string>

namespace av::session {

struct VideoSendLoopState {
    VideoSendSource source{VideoSendSource::None};
    bool sharingEnabled{false};
    bool cameraSendingEnabled{false};
    std::shared_ptr<av::capture::ScreenCapture> screenCapture;
    std::shared_ptr<av::capture::ScreenCapture> cameraFallbackCapture;
    std::shared_ptr<CameraFrameRelay> cameraRelay;
    bool cameraCaptureRunning{false};
    std::string preferredCameraName;
    bool peerReady{false};
};

struct VideoSendLoopSnapshot {
    VideoSendSourceSnapshot sourceSnapshot;
    bool peerReady{false};
};

class VideoSendLoopPipeline {
public:
    VideoSendLoopSnapshot makeSnapshot(VideoSendLoopState state) const;
};

}  // namespace av::session
