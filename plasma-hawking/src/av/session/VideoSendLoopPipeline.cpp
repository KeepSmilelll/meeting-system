#include "VideoSendLoopPipeline.h"

namespace av::session {

VideoSendLoopSnapshot VideoSendLoopPipeline::makeSnapshot(VideoSendLoopState state) const {
    VideoSendLoopSnapshot snapshot{};
    snapshot.peerReady = state.peerReady;
    snapshot.sourceSnapshot.source = state.source;
    snapshot.sourceSnapshot.sharingEnabled = state.sharingEnabled;
    snapshot.sourceSnapshot.cameraSendingEnabled = state.cameraSendingEnabled;

    if (state.source == VideoSendSource::Screen) {
        snapshot.sourceSnapshot.screenCapture = std::move(state.screenCapture);
        return snapshot;
    }

    if (state.source != VideoSendSource::Camera) {
        return snapshot;
    }

    if (state.cameraCaptureRunning) {
        snapshot.sourceSnapshot.cameraRelay = std::move(state.cameraRelay);
        snapshot.sourceSnapshot.cameraCaptureRunning = true;
    } else {
        snapshot.sourceSnapshot.cameraFallbackCapture = std::move(state.cameraFallbackCapture);
    }
    snapshot.sourceSnapshot.preferredCameraName = std::move(state.preferredCameraName);
    return snapshot;
}

}  // namespace av::session
