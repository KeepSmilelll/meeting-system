#include "VideoSendSourcePipeline.h"

#include <chrono>

namespace av::session {
namespace {

constexpr uint64_t kCameraFrameWarnTimeoutMs = 2000U;
constexpr uint64_t kCameraFrameAbortTimeoutMs = 3000U;

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

VideoSendFrameFetchResult VideoSendSourcePipeline::pullFrame(const VideoSendSourceSnapshot& snapshot,
                                                             std::chrono::milliseconds waitTimeout,
                                                             CameraFrameTimeoutState& timeoutState,
                                                             VideoSendPipelineInputFrame& outFrame,
                                                             std::string* statusMessage) const {
    if (statusMessage != nullptr) {
        statusMessage->clear();
    }
    outFrame.avFrame.reset();
    outFrame.screenFrame = av::capture::ScreenFrame{};

    if (snapshot.source == VideoSendSource::Screen) {
        if (!snapshot.screenCapture ||
            !snapshot.screenCapture->popFrameForEncode(outFrame.screenFrame, waitTimeout)) {
            return VideoSendFrameFetchResult::Retry;
        }
        return snapshot.sharingEnabled ? VideoSendFrameFetchResult::Ready
                                       : VideoSendFrameFetchResult::Retry;
    }

    if (snapshot.source == VideoSendSource::Camera) {
        if (snapshot.cameraRelay) {
            CameraFrameForEncode cameraFrame;
            if (!snapshot.cameraRelay->popFrameForEncode(cameraFrame, waitTimeout)) {
                const uint64_t nowMs = steadyNowMs();
                if (statusMessage != nullptr &&
                    shouldReportCameraFrameTimeout(timeoutState, nowMs, kCameraFrameWarnTimeoutMs)) {
                    *statusMessage = "Video camera frame timeout: capture_running=" +
                        std::to_string(snapshot.cameraCaptureRunning ? 1 : 0) +
                        " fallback=" + std::to_string(snapshot.cameraFallbackCapture ? 1 : 0) +
                        " preferred=" + (snapshot.preferredCameraName.empty()
                                            ? std::string("<default>")
                                            : snapshot.preferredCameraName);
                }
                if (timeoutState.waitStartedAtMs != 0 &&
                    nowMs - timeoutState.waitStartedAtMs >= kCameraFrameAbortTimeoutMs) {
                    if (statusMessage != nullptr) {
                        *statusMessage = "camera capture produced no frames: capture_running=" +
                            std::to_string(snapshot.cameraCaptureRunning ? 1 : 0) +
                            " fallback=" + std::to_string(snapshot.cameraFallbackCapture ? 1 : 0) +
                            " preferred=" + (snapshot.preferredCameraName.empty()
                                                ? std::string("<default>")
                                                : snapshot.preferredCameraName);
                    }
                    return VideoSendFrameFetchResult::Abort;
                }
                return VideoSendFrameFetchResult::Retry;
            }
            outFrame.avFrame = std::move(cameraFrame.avFrame);
            outFrame.screenFrame = std::move(cameraFrame.screenFrame);
            resetCameraFrameTimeout(timeoutState);
        } else if (snapshot.cameraFallbackCapture) {
            if (!snapshot.cameraFallbackCapture->popFrameForEncode(outFrame.screenFrame, waitTimeout)) {
                return VideoSendFrameFetchResult::Retry;
            }
            resetCameraFrameTimeout(timeoutState);
        } else {
            return VideoSendFrameFetchResult::Abort;
        }

        if (snapshot.sharingEnabled || !snapshot.cameraSendingEnabled) {
            return VideoSendFrameFetchResult::Retry;
        }
        return VideoSendFrameFetchResult::Ready;
    }

    return VideoSendFrameFetchResult::Abort;
}

}  // namespace av::session
