#pragma once

#include "av/capture/ScreenCapture.h"
#include "av/codec/VideoDecoder.h"
#include "av/codec/VideoEncoder.h"
#include "net/media/RTCPHandler.h"
#include "net/media/RTPReceiver.h"
#include "net/media/RTPSender.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

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
    struct CameraFrameRelay;

#ifdef _WIN32
    bool openSocketLocked();
    void closeSocketLocked();
    bool resolvePeerLocked(sockaddr_in& outPeer) const;
    void setErrorLocked(std::string message);
    bool startCaptureLocked();
    void stopCaptureLocked();
    bool startCameraCaptureLocked();
    void stopCameraFallbackCaptureLocked();
    void stopCameraCaptureLocked();
    bool shouldAcceptSenderLocked(const sockaddr_in& from) const;
    void sendLoop();
    void recvLoop();
    bool handleRtcpFeedbackLocked(const uint8_t* data, std::size_t len);
    bool retransmitPacketLocked(uint16_t sequenceNumber);
    void cacheSentPacketLocked(uint16_t sequenceNumber, std::vector<uint8_t> packetBytes);
    static bool looksLikeRtcp(const uint8_t* data, std::size_t len);
    static uint16_t parseSequenceNumber(const std::vector<uint8_t>& packetBytes);
#endif

    ScreenShareSessionConfig m_config;
    mutable std::mutex m_mutex;
    std::thread m_sendThread;
    std::thread m_recvThread;
    std::string m_lastError;
    std::function<void(av::codec::DecodedVideoFrame)> m_decodedFrameCallback;
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
    av::codec::VideoDecoder m_decoder;
    media::RTPSender m_sender;
    media::RTPReceiver m_receiver;
    media::RTCPHandler m_rtcpHandler;
    static constexpr std::size_t kRetransmitCacheLimit = 512U;
    std::deque<std::pair<uint16_t, std::vector<uint8_t>>> m_sentPacketCache;

#ifdef _WIN32
    SOCKET m_socket{INVALID_SOCKET};
    sockaddr_in m_peer{};
    bool m_peerValid{false};
#endif
};

}  // namespace av::session
