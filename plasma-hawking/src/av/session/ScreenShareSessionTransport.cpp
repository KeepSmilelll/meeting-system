#include "ScreenShareSession.h"

#include "VideoRecvConsumePipeline.h"
#include "VideoRecvErrorPipeline.h"
#include "VideoRecvFrameDispatchPipeline.h"
#include "VideoRecvIngressPipeline.h"
#include "VideoRecvKeyFramePipeline.h"
#include "VideoRecvPipeline.h"
#include "VideoRecvRtcpPipeline.h"
#include "VideoRecvTelemetryPipeline.h"
#include "VideoSendControlActions.h"
#include "VideoSendLoopPipeline.h"
#include "VideoSendPipeline.h"
#include "VideoSendSourcePipeline.h"
#include "VideoSendTelemetryPipeline.h"
#include "VideoSessionStateMachine.h"

#include "net/media/SocketAddressUtils.h"

#include <QDebug>
#include <QByteArray>
#include <QMutexLocker>
#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <limits>
#include <unordered_map>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace av::session {
namespace {

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool looksLikeStunPacket(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 20U) {
        return false;
    }
    if ((data[0] & 0xC0U) != 0U) {
        return false;
    }
    return data[4] == 0x21U && data[5] == 0x12U && data[6] == 0xA4U && data[7] == 0x42U;
}

constexpr std::size_t kMaxRecvPeerWorkers = 16U;
constexpr std::size_t kMaxRecvPeerQueuedPackets = 64U;
constexpr uint64_t kRecvPeerWorkerIdleTimeoutMs = 30 * 1000U;
constexpr std::size_t kRecvWorkerPollBudgetAfterPacket = 2U;
constexpr std::size_t kRecvWorkerPollBudgetWhenIdle = 2U;
constexpr std::size_t kRecvWorkerPollBudgetOnStop = 8U;

class VideoRecvPeerRuntime final {
public:
    using PacketOutcomeCallback = std::function<void(const media::RTPPacket&,
                                                     const VideoRecvConsumeOutcome&,
                                                     av::codec::DecodedVideoFrame&&)>;
    using PollOutcomeCallback = std::function<void(const VideoRecvConsumeOutcome&,
                                                   av::codec::DecodedVideoFrame&&)>;

    VideoRecvPeerRuntime(uint32_t remoteMediaSsrc,
                         VideoRecvPipelineConfig recvPipelineConfig,
                         PacketOutcomeCallback packetOutcomeCallback,
                         PollOutcomeCallback pollOutcomeCallback)
        : m_remoteMediaSsrc(remoteMediaSsrc),
          m_recvPipeline(std::move(recvPipelineConfig)),
          m_packetOutcomeCallback(std::move(packetOutcomeCallback)),
          m_pollOutcomeCallback(std::move(pollOutcomeCallback)),
          m_lastActiveAtMs(steadyNowMs()) {}

    ~VideoRecvPeerRuntime() {
        stop();
    }

    void start() {
        QMutexLocker locker(&m_mutex);
        if (m_thread != nullptr) {
            return;
        }
        m_stopping.store(false, std::memory_order_release);
        m_thread = QThread::create([this]() { run(); });
        if (m_thread != nullptr) {
            m_thread->start();
        }
    }

    void stop() {
        QThread* threadToJoin = nullptr;
        {
            QMutexLocker locker(&m_mutex);
            if (m_thread == nullptr) {
                return;
            }
            m_stopping.store(true, std::memory_order_release);
            m_waitCondition.wakeAll();
            threadToJoin = m_thread;
            m_thread = nullptr;
        }
        threadToJoin->wait();
        delete threadToJoin;

        QMutexLocker locker(&m_mutex);
        m_pendingPackets.clear();
    }

    bool enqueue(media::RTPPacket packet) {
        QMutexLocker locker(&m_mutex);
        if (m_thread == nullptr || m_stopping.load(std::memory_order_acquire)) {
            return false;
        }
        if (m_pendingPackets.size() >= kMaxRecvPeerQueuedPackets) {
            m_pendingPackets.pop_front();
        }
        m_pendingPackets.push_back(std::move(packet));
        m_lastActiveAtMs.store(steadyNowMs(), std::memory_order_release);
        m_waitCondition.wakeOne();
        return true;
    }

    uint32_t remoteMediaSsrc() const {
        return m_remoteMediaSsrc;
    }

    uint64_t lastActiveAtMs() const {
        return m_lastActiveAtMs.load(std::memory_order_acquire);
    }

private:
    void run() {
        while (true) {
            media::RTPPacket packet;
            bool hasPacket = false;
            {
                QMutexLocker locker(&m_mutex);
                if (m_pendingPackets.empty() &&
                    !m_stopping.load(std::memory_order_acquire)) {
                    m_waitCondition.wait(&m_mutex, 20);
                }
                if (m_stopping.load(std::memory_order_acquire) &&
                    m_pendingPackets.empty()) {
                    break;
                }
                if (!m_pendingPackets.empty()) {
                    packet = std::move(m_pendingPackets.front());
                    m_pendingPackets.pop_front();
                    hasPacket = true;
                }
            }

            if (hasPacket) {
                processPacket(packet);
            } else {
                pollDecodedFrames(kRecvWorkerPollBudgetWhenIdle);
            }
        }

        pollDecodedFrames(kRecvWorkerPollBudgetOnStop);
    }

    void processPacket(const media::RTPPacket& packet) {
        av::codec::DecodedVideoFrame decoded;
        VideoRecvConsumeOutcome consumeOutcome;
        if (!m_recvConsumePipeline.consumeAndDecide(
                packet,
                m_remoteMediaSsrc,
                m_recvPipeline,
                decoded,
                consumeOutcome)) {
            return;
        }
        m_lastActiveAtMs.store(steadyNowMs(), std::memory_order_release);
        if (m_packetOutcomeCallback) {
            m_packetOutcomeCallback(packet, consumeOutcome, std::move(decoded));
        }
        pollDecodedFrames(kRecvWorkerPollBudgetAfterPacket);
    }

    void pollDecodedFrames(std::size_t budget) {
        for (std::size_t index = 0; index < budget; ++index) {
            av::codec::DecodedVideoFrame decoded;
            VideoRecvConsumeOutcome consumeOutcome;
            if (!m_recvConsumePipeline.pollAndDecide(
                    m_recvPipeline,
                    decoded,
                    consumeOutcome)) {
                break;
            }
            m_lastActiveAtMs.store(steadyNowMs(), std::memory_order_release);
            if (m_pollOutcomeCallback) {
                m_pollOutcomeCallback(consumeOutcome, std::move(decoded));
            }
        }
    }

    const uint32_t m_remoteMediaSsrc{0U};
    VideoRecvConsumePipeline m_recvConsumePipeline;
    VideoRecvPipeline m_recvPipeline;
    PacketOutcomeCallback m_packetOutcomeCallback;
    PollOutcomeCallback m_pollOutcomeCallback;

    mutable QMutex m_mutex;
    QWaitCondition m_waitCondition;
    std::deque<media::RTPPacket> m_pendingPackets;
    QThread* m_thread{nullptr};
    std::atomic<bool> m_stopping{false};
    std::atomic<uint64_t> m_lastActiveAtMs{0};
};

}  // namespace

bool ScreenShareSession::openSocketLocked() {
    std::string socketError;
    if (!m_mediaSocket.open(m_config.localAddress, m_config.localPort, &socketError)) {
        setErrorLocked(socketError.empty() ? "socket setup failed" : socketError);
        closeSocketLocked();
        return false;
    }

    // Real dual-camera 1080p can burst RTP packets fast enough to overflow default UDP buffers.
    // Keep larger buffers by default and allow env overrides for machine-specific tuning.
    const auto socketBufferBytesFromEnv = [](const char* key, int fallbackBytes) {
        const int raw = qEnvironmentVariableIntValue(key);
        const int configured = raw > 0 ? raw : fallbackBytes;
        return std::clamp(configured, 64 * 1024, 16 * 1024 * 1024);
    };
    const int recvBufferBytes = socketBufferBytesFromEnv("MEETING_VIDEO_SOCKET_RECVBUF_BYTES", 4 * 1024 * 1024);
    const int sendBufferBytes = socketBufferBytesFromEnv("MEETING_VIDEO_SOCKET_SNDBUF_BYTES", 4 * 1024 * 1024);
    std::string socketOptionError;
    if (!m_mediaSocket.configureSocketBuffers(recvBufferBytes, sendBufferBytes, &socketOptionError)) {
        setErrorLocked(socketOptionError.empty() ? "socket buffer setup failed" : socketOptionError);
    }

    (void)m_mediaSocket.setPeer(m_config.peerAddress, m_config.peerPort);
    m_mediaSocket.setReadTimeoutMs(50);
    return true;
}

void ScreenShareSession::closeSocketLocked() {
    m_mediaSocket.close();
}

bool ScreenShareSession::applyRtcpDispatchPlanLocked(
    const VideoRtcpFeedbackDispatchPlan& dispatchPlan) {
    if (!dispatchPlan.hasActions()) {
        return false;
    }

    bool handled = false;
    for (const uint16_t sequenceNumber : dispatchPlan.retransmitSequenceNumbers) {
        std::string retransmitError;
        if (!m_rtcpActionPipeline.retransmitPacket(sequenceNumber, m_mediaSocket, &retransmitError)) {
            if (!retransmitError.empty()) {
                setErrorLocked(retransmitError);
            }
            continue;
        }
        m_retransmitPacketCount.fetch_add(1, std::memory_order_acq_rel);
        handled = true;
    }

    if (dispatchPlan.requestKeyFrame) {
        m_forceKeyFramePending.store(true, std::memory_order_release);
        m_keyframeRequestCount.fetch_add(1, std::memory_order_acq_rel);
        handled = true;
    }

    if (dispatchPlan.hasTargetBitrate) {
        const uint32_t previousTarget =
            m_targetBitrateBps.exchange(dispatchPlan.targetBitrateBps, std::memory_order_acq_rel);
        if (previousTarget != dispatchPlan.targetBitrateBps) {
            m_targetBitrateUpdatedAtMs.store(steadyNowMs(), std::memory_order_release);
        }
        handled = true;
    }

    return handled;
}

void ScreenShareSession::setErrorLocked(std::string message) {
    m_lastError = std::move(message);
    qWarning().noquote() << "[screen-session]" << QString::fromStdString(m_lastError);
    if (m_statusCallback) {
        m_statusCallback(QStringLiteral("Video session error: %1")
                             .arg(QString::fromStdString(m_lastError))
                             .toStdString());
    }
}

void ScreenShareSession::captureLoop() {
    const VideoSendSourcePipeline sendSourcePipeline;
    const VideoSendLoopPipeline sendLoopPipeline;
    CameraFrameTimeoutState cameraFrameTimeoutState;
    const auto statusCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_statusCallback;
    }();
    const auto errorCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_errorCallback;
    }();

    while (m_running.load(std::memory_order_acquire) &&
           (m_sharingEnabled.load(std::memory_order_acquire) ||
            m_cameraSendingEnabled.load(std::memory_order_acquire))) {
        {
            QMutexLocker locker(&m_mutex);
            while (m_running.load(std::memory_order_acquire) &&
                   (m_sharingEnabled.load(std::memory_order_acquire) ||
                    m_cameraSendingEnabled.load(std::memory_order_acquire)) &&
                   !m_mediaSocket.hasPeer()) {
                m_stateWaitCondition.wait(&m_mutex, 100);
            }
            if (!m_running.load(std::memory_order_acquire) ||
                (!m_sharingEnabled.load(std::memory_order_acquire) &&
                 !m_cameraSendingEnabled.load(std::memory_order_acquire))) {
                break;
            }
        }

        const VideoSendSource source = VideoSessionStateMachine::resolveSendSource(
            m_sharingEnabled.load(std::memory_order_acquire),
            m_cameraSendingEnabled.load(std::memory_order_acquire));

        VideoSendLoopState loopState{};
        loopState.source = source;
        {
            QMutexLocker locker(&m_mutex);
            loopState.sharingEnabled = m_sharingEnabled.load(std::memory_order_acquire);
            loopState.cameraSendingEnabled = m_cameraSendingEnabled.load(std::memory_order_acquire);
            loopState.screenCapture = m_capture;
            loopState.cameraRelay = m_cameraRelay;
            loopState.cameraFallbackCapture = m_cameraFallbackCapture;
            loopState.cameraCaptureRunning = m_cameraCapture && m_cameraCapture->isRunning();
            loopState.preferredCameraName = m_preferredCameraDeviceName;
            loopState.peerReady = m_mediaSocket.hasPeer();
        }
        const VideoSendLoopSnapshot loopSnapshot = sendLoopPipeline.makeSnapshot(std::move(loopState));

        VideoSendPipelineInputFrame frame;
        std::string sourceStatusMessage;
        const VideoSendFrameFetchResult frameFetchResult = sendSourcePipeline.pullFrame(
            loopSnapshot.sourceSnapshot,
            std::chrono::milliseconds(100),
            cameraFrameTimeoutState,
            frame,
            &sourceStatusMessage);
        if (!sourceStatusMessage.empty() && statusCallback) {
            statusCallback(sourceStatusMessage);
        }
        if (frameFetchResult == VideoSendFrameFetchResult::Retry) {
            continue;
        }
        if (frameFetchResult == VideoSendFrameFetchResult::Abort) {
            if (source == VideoSendSource::Camera) {
                const std::string errorMessage = sourceStatusMessage.empty()
                    ? std::string{"camera capture produced no frames"}
                    : sourceStatusMessage;
                {
                    QMutexLocker locker(&m_mutex);
                    setErrorLocked(errorMessage);
                    m_cameraSendingEnabled.store(false, std::memory_order_release);
                    stopCameraCaptureLocked();
                }
                if (errorCallback) {
                    errorCallback(errorMessage);
                }
            }
            break;
        }

        VideoSendCapturedFrame capturedFrame;
        capturedFrame.source = source;
        capturedFrame.inputFrame = std::move(frame);
        if (!m_sendFrameRingBuffer.push(std::move(capturedFrame))) {
            break;
        }
    }

    m_sendFrameRingBuffer.close();
}

void ScreenShareSession::sendLoop() {
    av::codec::VideoEncoder encoder;
    const VideoSendPipeline sendPipeline(
        VideoSendPipelineConfig{m_config.frameRate, m_config.maxPayloadBytes});
    if (!encoder.configure(m_config.width,
                           m_config.height,
                           m_config.frameRate,
                           m_config.bitrate,
                           m_config.cameraPayloadType,
                           m_config.encoderPreset)) {
        std::function<void(std::string)> errorCallback;
        {
            QMutexLocker locker(&m_mutex);
            setErrorLocked("video encoder configure failed");
            errorCallback = m_errorCallback;
        }
        if (errorCallback) {
            errorCallback("video encoder configure failed");
        }
        return;
    }
    m_appliedBitrateBps.store(static_cast<uint32_t>(encoder.bitrate()), std::memory_order_release);

    VideoSendTelemetryPipeline sendTelemetryPipeline;
    VideoSendTelemetryState sendTelemetryState;
    const auto statusCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_statusCallback;
    }();
    while (m_running.load(std::memory_order_acquire) &&
           (m_sharingEnabled.load(std::memory_order_acquire) ||
            m_cameraSendingEnabled.load(std::memory_order_acquire))) {
        {
            QMutexLocker locker(&m_mutex);
            while (m_running.load(std::memory_order_acquire) &&
                   (m_sharingEnabled.load(std::memory_order_acquire) ||
                    m_cameraSendingEnabled.load(std::memory_order_acquire)) &&
                   !m_mediaSocket.hasPeer()) {
                m_stateWaitCondition.wait(&m_mutex, 100);
            }
            if (!m_running.load(std::memory_order_acquire) ||
                (!m_sharingEnabled.load(std::memory_order_acquire) &&
                 !m_cameraSendingEnabled.load(std::memory_order_acquire))) {
                break;
            }
        }

        VideoSendCapturedFrame capturedFrame;
        if (!m_sendFrameRingBuffer.popWait(capturedFrame, std::chrono::milliseconds(100))) {
            if (m_sendFrameRingBuffer.closed()) {
                break;
            }
            continue;
        }
        const VideoSendSource source = capturedFrame.source;
        if (source == VideoSendSource::None) {
            continue;
        }
        const VideoSendSource currentSource = VideoSessionStateMachine::resolveSendSource(
            m_sharingEnabled.load(std::memory_order_acquire),
            m_cameraSendingEnabled.load(std::memory_order_acquire));
        if (currentSource != source) {
            continue;
        }
        media::UdpEndpoint peer{};
        {
            QMutexLocker locker(&m_mutex);
            if (m_mediaSocket.hasPeer()) {
                peer = m_mediaSocket.peer();
            }
        }
        if (!peer.isValid()) {
            continue;
        }

        std::string bitrateError;
        if (!maybeApplyTargetBitrate(encoder,
                                     m_targetBitrateBps,
                                     m_appliedBitrateBps,
                                     m_bitrateReconfigureCount,
                                     m_targetBitrateUpdatedAtMs,
                                     m_lastBitrateApplyDelayMs,
                                     steadyNowMs(),
                                     &bitrateError)) {
            QMutexLocker locker(&m_mutex);
            setErrorLocked(bitrateError.empty() ? "video encoder bitrate update failed" : bitrateError);
            continue;
        }

        const bool forceKeyFrame = m_forceKeyFramePending.exchange(false, std::memory_order_acq_rel);
        std::vector<VideoSendPipelinePacket> packets;
        std::string pipelineError;
        bool encodedKeyFrame = false;
        const uint8_t payloadType = source == VideoSendSource::Screen ? m_config.payloadType
                                                                      : m_config.cameraPayloadType;
        if (!sendPipeline.encodeAndPacketize(encoder,
                                             capturedFrame.inputFrame,
                                             payloadType,
                                             forceKeyFrame,
                                             m_sender,
                                             packets,
                                             &encodedKeyFrame,
                                             &pipelineError)) {
            if (pipelineError.empty()) {
                sendTelemetryPipeline.onEncodePending(sendTelemetryState, statusCallback);
                continue;
            }
            sendTelemetryPipeline.onEncodeError(sendTelemetryState, pipelineError, statusCallback);
            QMutexLocker locker(&m_mutex);
            setErrorLocked(pipelineError);
            continue;
        }
        sendTelemetryPipeline.onEncodedPacketObserved(sendTelemetryState, statusCallback);
        if (forceKeyFrame && !encodedKeyFrame) {
            m_forceKeyFramePending.store(true, std::memory_order_release);
        }

        for (const auto& packet : packets) {
            {
                QMutexLocker locker(&m_mutex);
                if (m_dtlsStarted.load(std::memory_order_acquire) &&
                    !m_srtpReady.load(std::memory_order_acquire)) {
                    (void)sendCachedDtlsHandshakeLocked(peer);
                    continue;
                }
            }

            std::vector<uint8_t> outboundPacket = packet.bytes;
            {
                QMutexLocker locker(&m_mutex);
                if (!protectRtpLocked(&outboundPacket)) {
                    continue;
                }
            }
            const int sent = m_mediaSocket.sendTo(outboundPacket.data(), outboundPacket.size(), peer);
            if (sent != static_cast<int>(outboundPacket.size())) {
                QMutexLocker locker(&m_mutex);
                setErrorLocked("sendto failed");
                break;
            }
            m_sentPacketCount.fetch_add(1, std::memory_order_acq_rel);
            {
                QMutexLocker locker(&m_mutex);
                m_rtcpActionPipeline.cacheSentPacket(packet.sequenceNumber, outboundPacket);
            }
            sendTelemetryPipeline.onPacketSent(sendTelemetryState, packet, statusCallback);
        }
    }
}

void ScreenShareSession::recvLoop() {
    std::array<uint8_t, 1500> buffer{};
    bool loggedFirstPacket = false;
    const auto statusCallback = [this]() {
        QMutexLocker locker(&m_mutex);
        return m_statusCallback;
    }();
    bool loggedFirstDecodedFrame = false;
    VideoRecvConsumePipeline recvConsumePipeline;
    VideoRecvErrorPipeline recvErrorPipeline;
    VideoRecvFrameDispatchPipeline frameDispatchPipeline;
    VideoRecvIngressPipeline recvIngressPipeline;
    VideoRecvRtcpPipeline recvRtcpPipeline;
    VideoRecvTelemetryPipeline recvTelemetryPipeline;
    const VideoRecvPipelineConfig recvPipelineConfig{
        m_config.payloadType,
        m_config.cameraPayloadType,
        m_config.frameRate};
    VideoRecvPipeline datagramPipeline(recvPipelineConfig);
    VideoRecvKeyFramePipeline keyFramePipeline;
    std::unordered_map<uint32_t, std::unique_ptr<VideoRecvPeerRuntime>> recvPeerWorkers;
    QMutex consumeActionMutex;
    QMutex telemetryMutex;
    QMutex pendingKeyFrameMutex;
    uint64_t lastPliRequestedAtMs = 0;
    struct PendingKeyFrameRequest {
        uint32_t remoteMediaSsrc{0U};
        std::string reason;
    };
    std::deque<PendingKeyFrameRequest> pendingKeyFrameRequests;

    const auto stopAllRecvPeerWorkers = [&recvPeerWorkers]() {
        for (auto& [remoteMediaSsrc, worker] : recvPeerWorkers) {
            (void)remoteMediaSsrc;
            if (worker) {
                worker->stop();
            }
        }
        recvPeerWorkers.clear();
    };

    const auto pruneRecvPeerWorkers = [&recvPeerWorkers](uint64_t nowMs,
                                                         uint32_t expectedRemoteSsrc) {
        for (auto it = recvPeerWorkers.begin(); it != recvPeerWorkers.end();) {
            const uint32_t remoteMediaSsrc = it->first;
            const bool keepExpectedOnly =
                expectedRemoteSsrc != 0U && remoteMediaSsrc != expectedRemoteSsrc;
            const bool idleExpired =
                expectedRemoteSsrc == 0U &&
                nowMs >= it->second->lastActiveAtMs() &&
                (nowMs - it->second->lastActiveAtMs()) > kRecvPeerWorkerIdleTimeoutMs;
            if (!keepExpectedOnly && !idleExpired) {
                ++it;
                continue;
            }
            if (it->second) {
                it->second->stop();
            }
            it = recvPeerWorkers.erase(it);
        }
    };

    const auto enqueueKeyFrameRequest = [&pendingKeyFrameMutex, &pendingKeyFrameRequests](
                                            uint32_t remoteMediaSsrc,
                                            std::string reason) {
        if (remoteMediaSsrc == 0U) {
            return;
        }
        QMutexLocker locker(&pendingKeyFrameMutex);
        if (pendingKeyFrameRequests.size() >= 64U) {
            pendingKeyFrameRequests.pop_front();
        }
        pendingKeyFrameRequests.push_back(PendingKeyFrameRequest{
            remoteMediaSsrc,
            std::move(reason),
        });
    };

    const auto flushPendingKeyFrameRequests =
        [this,
         &pendingKeyFrameMutex,
         &pendingKeyFrameRequests,
         &keyFramePipeline,
         &lastPliRequestedAtMs,
         &statusCallback]() {
            std::deque<PendingKeyFrameRequest> requests;
            {
                QMutexLocker locker(&pendingKeyFrameMutex);
                requests.swap(pendingKeyFrameRequests);
            }

            for (auto& request : requests) {
                if (request.remoteMediaSsrc == 0U) {
                    continue;
                }

                const uint64_t nowMs = steadyNowMs();
                if (!keyFramePipeline.shouldSendPli(
                        request.remoteMediaSsrc, nowMs, lastPliRequestedAtMs)) {
                    continue;
                }

                bool pliSent = false;
                {
                    QMutexLocker locker(&m_mutex);
                    std::string pliError;
                    pliSent = m_rtcpActionPipeline.sendPictureLossIndication(
                        m_mediaSocket, m_sender.ssrc(), request.remoteMediaSsrc, &pliError);
                    if (!pliSent && !pliError.empty()) {
                        setErrorLocked(pliError);
                    }
                }

                if (!pliSent) {
                    continue;
                }

                keyFramePipeline.markPliSent(nowMs, lastPliRequestedAtMs);
                m_keyframeRequestCount.fetch_add(1, std::memory_order_acq_rel);
                if (statusCallback) {
                    statusCallback(std::string("Video keyframe requested: ") + request.reason);
                }
            }
        };

    const auto handleConsumeOutcome =
        [this,
         &recvErrorPipeline,
         &frameDispatchPipeline,
         &enqueueKeyFrameRequest,
         &statusCallback,
         &consumeActionMutex,
         &loggedFirstDecodedFrame](const VideoRecvConsumeOutcome& consumeOutcome,
                                   av::codec::DecodedVideoFrame decoded) {
            QMutexLocker consumeLocker(&consumeActionMutex);
            const VideoRecvHandlingDecision& decision = consumeOutcome.decision;
            if (decision.action == VideoRecvHandlingAction::Continue) {
                return;
            }

            if (decision.action == VideoRecvHandlingAction::RequestKeyFrame ||
                decision.action == VideoRecvHandlingAction::RequestKeyFrameAndError) {
                const char* keyFrameReason =
                    decision.keyFrameReason != nullptr ? decision.keyFrameReason : "decode failure";
                enqueueKeyFrameRequest(consumeOutcome.remoteMediaSsrc, keyFrameReason);

                std::string decisionError;
                if (recvErrorPipeline.extractDecisionError(decision, decisionError)) {
                    QMutexLocker locker(&m_mutex);
                    setErrorLocked(decisionError);
                }
                return;
            }

            if (decision.action != VideoRecvHandlingAction::DeliverFrame) {
                return;
            }

            std::function<void(av::codec::DecodedVideoFrame)> callback;
            std::function<void(av::codec::DecodedVideoFrame, uint32_t)> callbackWithSsrc;
            {
                QMutexLocker locker(&m_mutex);
                callback = m_decodedFrameCallback;
                callbackWithSsrc = m_decodedFrameWithSsrcCallback;
            }
            frameDispatchPipeline.reportFirstDecodedFrame(
                loggedFirstDecodedFrame, decoded, statusCallback);
            frameDispatchPipeline.dispatchFrame(
                callback,
                callbackWithSsrc,
                std::move(decoded),
                consumeOutcome.remoteMediaSsrc);
        };

    const auto ensureRecvPeerWorker =
        [&recvPeerWorkers,
         &recvPipelineConfig,
         &handleConsumeOutcome,
         &recvTelemetryPipeline,
         &telemetryMutex,
         &loggedFirstPacket,
         &statusCallback,
         this](uint32_t remoteMediaSsrc) -> VideoRecvPeerRuntime* {
            if (remoteMediaSsrc == 0U) {
                return nullptr;
            }

            auto found = recvPeerWorkers.find(remoteMediaSsrc);
            if (found != recvPeerWorkers.end()) {
                return found->second.get();
            }

            if (recvPeerWorkers.size() >= kMaxRecvPeerWorkers) {
                auto oldest = recvPeerWorkers.end();
                uint64_t oldestActiveAtMs = (std::numeric_limits<uint64_t>::max)();
                for (auto it = recvPeerWorkers.begin(); it != recvPeerWorkers.end(); ++it) {
                    const uint64_t activeAtMs = it->second->lastActiveAtMs();
                    if (activeAtMs < oldestActiveAtMs) {
                        oldestActiveAtMs = activeAtMs;
                        oldest = it;
                    }
                }
                if (oldest != recvPeerWorkers.end()) {
                    if (oldest->second) {
                        oldest->second->stop();
                    }
                    recvPeerWorkers.erase(oldest);
                }
            }

            auto worker = std::make_unique<VideoRecvPeerRuntime>(
                remoteMediaSsrc,
                recvPipelineConfig,
                [this, &recvTelemetryPipeline, &telemetryMutex, &loggedFirstPacket, &statusCallback, &handleConsumeOutcome](
                    const media::RTPPacket& packet,
                    const VideoRecvConsumeOutcome& consumeOutcome,
                    av::codec::DecodedVideoFrame&& decoded) mutable {
                    {
                        QMutexLocker telemetryLocker(&telemetryMutex);
                        recvTelemetryPipeline.onPacketAccepted(
                            m_receivedPacketCount,
                            loggedFirstPacket,
                            packet,
                            statusCallback);
                    }
                    handleConsumeOutcome(consumeOutcome, std::move(decoded));
                },
                [&handleConsumeOutcome](const VideoRecvConsumeOutcome& consumeOutcome,
                                        av::codec::DecodedVideoFrame&& decoded) mutable {
                    handleConsumeOutcome(consumeOutcome, std::move(decoded));
                });
            worker->start();
            VideoRecvPeerRuntime* workerRaw = worker.get();
            recvPeerWorkers.emplace(remoteMediaSsrc, std::move(worker));
            return workerRaw;
        };

    while (m_running.load(std::memory_order_acquire)) {
        flushPendingKeyFrameRequests();
        const uint32_t expectedRemoteSsrc =
            m_expectedRemoteVideoSsrc.load(std::memory_order_acquire);
        pruneRecvPeerWorkers(steadyNowMs(), expectedRemoteSsrc);

        const int waitResult = m_mediaSocket.waitForReadable(-1);
        if (waitResult < 0) {
            const bool transientSocketError = m_mediaSocket.isTransientSocketError();
            if (!recvErrorPipeline.shouldReportSocketError(
                    m_running.load(std::memory_order_acquire), transientSocketError)) {
                continue;
            }
            QMutexLocker locker(&m_mutex);
            setErrorLocked("wait readable failed");
            continue;
        }

        if (waitResult > 0) {
            media::UdpEndpoint from{};
            const int received = m_mediaSocket.recvFrom(buffer.data(), buffer.size(), from);
            if (received <= 0) {
                const bool transientSocketError = m_mediaSocket.isTransientSocketError();
                if (!recvErrorPipeline.shouldReportSocketError(
                        m_running.load(std::memory_order_acquire), transientSocketError)) {
                    continue;
                }
                QMutexLocker locker(&m_mutex);
                setErrorLocked("recvfrom failed");
                continue;
            }

            const QByteArray receivedDatagram(reinterpret_cast<const char*>(buffer.data()), received);
            if (media::DtlsTransportClient::looksLikeDtlsRecord(receivedDatagram)) {
                QMutexLocker locker(&m_mutex);
                (void)handleDtlsPacketLocked(buffer.data(), static_cast<std::size_t>(received), from);
                continue;
            }
            if (looksLikeStunPacket(buffer.data(), static_cast<std::size_t>(received))) {
                m_iceConnected.store(true, std::memory_order_release);
                if (statusCallback) {
                    statusCallback("Video ice-connected binding-response received");
                }
                continue;
            }

            std::vector<uint8_t> mediaPacket(buffer.begin(), buffer.begin() + received);
            if (m_dtlsStarted.load(std::memory_order_acquire)) {
                QMutexLocker locker(&m_mutex);
                const bool looksLikeRtcp = media::looksLikeRtcpPacket(mediaPacket.data(), mediaPacket.size());
                const bool unprotected = looksLikeRtcp
                    ? unprotectRtcpLocked(&mediaPacket)
                    : unprotectRtpLocked(&mediaPacket);
                if (!unprotected) {
                    continue;
                }
            }

            const VideoRecvIngressGate ingressGate =
                recvIngressPipeline.evaluateGate(m_mediaSocket, from);

            VideoRecvDatagram datagram = datagramPipeline.classifyDatagram(
                mediaPacket.data(),
                mediaPacket.size(),
                ingressGate.acceptSender,
                ingressGate.acceptRtcpFromPeerHost,
                m_receiver);
            const VideoRecvIngressAction ingressAction =
                recvIngressPipeline.resolveEntryAction(datagram.kind);
            if (ingressAction == VideoRecvIngressAction::Rtcp) {
                uint32_t localSsrc = 0U;
                {
                    QMutexLocker locker(&m_mutex);
                    localSsrc = m_sender.ssrc();
                }

                VideoRtcpFeedbackDispatchPlan dispatchPlan;
                if (recvRtcpPipeline.parseDispatchPlan(
                        mediaPacket.data(),
                        mediaPacket.size(),
                        localSsrc,
                        m_rtcpFeedbackPipeline,
                        m_rtcpFeedbackDispatchPipeline,
                        dispatchPlan)) {
                    QMutexLocker locker(&m_mutex);
                    (void)applyRtcpDispatchPlanLocked(dispatchPlan);
                }
            } else if (ingressAction == VideoRecvIngressAction::Rtp) {
                const uint32_t packetSsrc = datagram.rtpPacket.header.ssrc;
                if (expectedRemoteSsrc != 0U && packetSsrc != expectedRemoteSsrc) {
                    continue;
                }
                VideoRecvPeerRuntime* worker = ensureRecvPeerWorker(packetSsrc);
                if (worker == nullptr) {
                    continue;
                }
                if (!worker->enqueue(std::move(datagram.rtpPacket))) {
                    QMutexLocker locker(&m_mutex);
                    setErrorLocked("recv worker enqueue failed");
                }
            }
        }
    }

    flushPendingKeyFrameRequests();
    stopAllRecvPeerWorkers();
}

}  // namespace av::session
