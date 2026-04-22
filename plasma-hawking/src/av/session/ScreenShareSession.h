#pragma once

#include "CameraFrameRelay.h"
#include "VideoThreadRuntime.h"
#include "VideoRtcpFeedbackPipeline.h"
#include "VideoRtcpFeedbackDispatchPipeline.h"
#include "VideoRtcpActionPipeline.h"
#include "VideoSendFrameRingBuffer.h"

#include "av/capture/ScreenCapture.h"
#include "av/codec/VideoDecoder.h"
#include "av/codec/VideoEncoder.h"
#include "net/media/RTPReceiver.h"
#include "net/media/RTPSender.h"
#include "net/media/UdpPeerSocket.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QMutex>
#include <QWaitCondition>

namespace av::capture {
class CameraCapture;
class ScreenCapture;
}

namespace av::session {

struct ScreenShareSessionConfig {
    std::string localAddress{"0.0.0.0"};
    uint16_t localPort{0};
    std::string peerAddress{"127.0.0.1"};
    uint16_t peerPort{0};
    int width{1280};
    int height{720};
    int frameRate{5};
    int bitrate{1500 * 1000};
    av::codec::VideoEncoderPreset encoderPreset{av::codec::VideoEncoderPreset::Realtime};
    uint8_t cameraPayloadType{96};
    uint8_t payloadType{97};
    std::size_t maxPayloadBytes{1200};
};

class ScreenShareSession {
public:
    explicit ScreenShareSession(ScreenShareSessionConfig config = {});
    ~ScreenShareSession();

    bool start();
    void stop();

    bool setSharingEnabled(bool enabled);
    bool sharingEnabled() const;
    bool setCameraSendingEnabled(bool enabled);
    bool cameraSendingEnabled() const;
    bool setPreferredCameraDeviceName(const std::string& deviceName);
    std::string preferredCameraDeviceName() const;
    void setExpectedRemoteVideoSsrc(uint32_t ssrc);
    uint32_t expectedRemoteVideoSsrc() const;

    void setPeer(const std::string& address, uint16_t port);
    void setDecodedFrameCallback(std::function<void(av::codec::DecodedVideoFrame)> callback);
    void setDecodedFrameWithSsrcCallback(
        std::function<void(av::codec::DecodedVideoFrame, uint32_t)> callback);
    void setErrorCallback(std::function<void(std::string)> callback);
    void setCameraSourceCallback(std::function<void(bool syntheticFallback)> callback);
    void setStatusCallback(std::function<void(std::string)> callback);
    uint16_t localPort() const;
    uint32_t videoSsrc() const;
    bool isRunning() const;
    std::string lastError() const;

    uint64_t sentPacketCount() const;
    uint64_t receivedPacketCount() const;
    uint64_t keyframeRequestCount() const;
    uint64_t retransmitPacketCount() const;
    uint64_t bitrateReconfigureCount() const;
    uint32_t lastBitrateApplyDelayMs() const;
    uint32_t targetBitrateBps() const;
    uint32_t appliedBitrateBps() const;

private:
    bool openSocketLocked();
    void closeSocketLocked();
    void setErrorLocked(std::string message);
    bool startCaptureLocked();
    void stopCaptureLocked();
    bool startCameraCaptureLocked();
    void stopCameraFallbackCaptureLocked();
    void stopCameraCaptureLocked();
    void captureLoop();
    void sendLoop();
    void recvLoop();
    bool applyRtcpDispatchPlanLocked(const VideoRtcpFeedbackDispatchPlan& dispatchPlan);

    ScreenShareSessionConfig m_config;
    mutable QMutex m_mutex;
    QWaitCondition m_stateWaitCondition;
    VideoThreadRuntime m_threadRuntime;
    std::string m_lastError;
    std::function<void(av::codec::DecodedVideoFrame)> m_decodedFrameCallback;
    std::function<void(av::codec::DecodedVideoFrame, uint32_t)> m_decodedFrameWithSsrcCallback;
    std::function<void(std::string)> m_errorCallback;
    std::function<void(bool)> m_cameraSourceCallback;
    std::function<void(std::string)> m_statusCallback;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_sharingEnabled{false};
    std::atomic<bool> m_cameraSendingEnabled{false};
    std::string m_preferredCameraDeviceName;
    std::atomic<uint64_t> m_sentPacketCount{0};
    std::atomic<uint64_t> m_receivedPacketCount{0};
    std::atomic<uint64_t> m_keyframeRequestCount{0};
    std::atomic<uint64_t> m_retransmitPacketCount{0};
    std::atomic<uint64_t> m_bitrateReconfigureCount{0};
    std::atomic<uint32_t> m_targetBitrateBps{0};
    std::atomic<uint32_t> m_appliedBitrateBps{0};
    std::atomic<uint32_t> m_lastBitrateApplyDelayMs{0};
    std::atomic<uint64_t> m_targetBitrateUpdatedAtMs{0};
    std::atomic<bool> m_forceKeyFramePending{false};
    std::atomic<uint32_t> m_expectedRemoteVideoSsrc{0};
    std::shared_ptr<av::capture::ScreenCapture> m_capture;
    std::unique_ptr<av::capture::CameraCapture> m_cameraCapture;
    std::shared_ptr<av::capture::ScreenCapture> m_cameraFallbackCapture;
    std::shared_ptr<CameraFrameRelay> m_cameraRelay;
    media::RTPSender m_sender;
    media::RTPReceiver m_receiver;
    VideoRtcpActionPipeline m_rtcpActionPipeline;
    VideoRtcpFeedbackPipeline m_rtcpFeedbackPipeline;
    VideoRtcpFeedbackDispatchPipeline m_rtcpFeedbackDispatchPipeline;
    VideoSendFrameRingBuffer m_sendFrameRingBuffer{4U};

    media::UdpPeerSocket m_mediaSocket;
};

}  // namespace av::session
