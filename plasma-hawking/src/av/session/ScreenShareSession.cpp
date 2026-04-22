#include "ScreenShareSession.h"

#include "VideoSessionControlActions.h"
#include "VideoSessionStateMachine.h"
#include "VideoThreadLifecycleStateMachine.h"

#include "net/media/SocketAddressUtils.h"

#include <QDebug>
#include <QMutexLocker>

#include <algorithm>
#include <chrono>
#include <functional>
#include <random>
#include <utility>

namespace av::session {
namespace {

uint32_t makeSsrc() {
    uint32_t value = 0;
    while (value == 0) {
        value = static_cast<uint32_t>(std::random_device{}());
    }
    return value;
}

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

ScreenShareSession::ScreenShareSession(ScreenShareSessionConfig config)
    : m_config(std::move(config)),
      m_cameraRelay(std::make_shared<CameraFrameRelay>(m_config.width, m_config.height, m_config.frameRate)),
      m_sender(0, 0) {
    m_targetBitrateBps.store(static_cast<uint32_t>((std::max)(1000, m_config.bitrate)), std::memory_order_release);
    m_appliedBitrateBps.store(static_cast<uint32_t>((std::max)(1000, m_config.bitrate)), std::memory_order_release);
    m_targetBitrateUpdatedAtMs.store(steadyNowMs(), std::memory_order_release);
}

ScreenShareSession::~ScreenShareSession() {
    stop();
}

bool ScreenShareSession::start() {
    const auto startPlan = VideoThreadLifecycleStateMachine::planSessionStart(
        m_running.load(std::memory_order_acquire));
    if (startPlan.alreadyRunning) {
        return true;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_lastError.clear();
        if (!openSocketLocked()) {
            return false;
        }
    }

    m_running.store(true, std::memory_order_release);
    m_stateWaitCondition.wakeAll();
    if (startPlan.shouldStartRecvThread) {
        m_threadRuntime.startRecv([this] { recvLoop(); }, false);
    }
    return true;
}

void ScreenShareSession::stop() {
    const bool wasRunning = m_running.exchange(false, std::memory_order_acq_rel);
    const auto stopPlan = VideoThreadLifecycleStateMachine::planSessionStop(
        wasRunning, m_threadRuntime.sendJoinable(), m_threadRuntime.recvJoinable());
    if (stopPlan.shouldReturnEarly) {
        return;
    }

    m_sharingEnabled.store(false, std::memory_order_release);
    m_cameraSendingEnabled.store(false, std::memory_order_release);
    m_stateWaitCondition.wakeAll();
    m_mediaSocket.interruptWaiters();
    m_sendFrameRingBuffer.close();
    m_forceKeyFramePending.store(false, std::memory_order_release);
    m_expectedRemoteVideoSsrc.store(0U, std::memory_order_release);
    if (m_cameraRelay) {
        m_cameraRelay->setSharingEnabled(false);
        m_cameraRelay->invalidate();
    }

    if (stopPlan.shouldJoinSendBeforeCleanup) {
        m_threadRuntime.joinCapture();
        m_threadRuntime.joinSend();
    } else if (m_threadRuntime.captureJoinable()) {
        m_threadRuntime.joinCapture();
    }
    if (stopPlan.shouldJoinRecvAfterCleanup) {
        m_threadRuntime.joinRecv();
    }

    {
        QMutexLocker locker(&m_mutex);
        stopCaptureLocked();
        stopCameraCaptureLocked();
        m_sender.setSSRC(0);
        m_rtcpActionPipeline.reset();
        closeSocketLocked();
    }
}

bool ScreenShareSession::setSharingEnabled(bool enabled) {
    if (enabled) {
        if (!m_running.load(std::memory_order_acquire) && !start()) {
            return false;
        }
        const auto enablePlan = VideoSessionStateMachine::planEnableSharing(
            m_sharingEnabled.load(std::memory_order_acquire),
            m_cameraSendingEnabled.load(std::memory_order_acquire));
        if (enablePlan.alreadyEnabled) {
            return true;
        }

        bool startSendThread = false;
        {
            QMutexLocker locker(&m_mutex);
            if (!startCaptureLocked()) {
                return false;
            }
            if (enablePlan.shouldStartSendThread) {
                resetSenderForFreshStream(m_sender, m_rtcpActionPipeline, makeSsrc());
                startSendThread = true;
            }
        }

        m_sharingEnabled.store(true, std::memory_order_release);
        if (m_cameraRelay) {
            m_cameraRelay->setSharingEnabled(true);
        }
        if (enablePlan.shouldRequestKeyFrame) {
            m_forceKeyFramePending.store(true, std::memory_order_release);
        }
        if (startSendThread) {
            resetSendThreadStats(m_keyframeRequestCount,
                                 m_retransmitPacketCount,
                                 m_bitrateReconfigureCount,
                                 m_targetBitrateBps,
                                 m_appliedBitrateBps,
                                 m_lastBitrateApplyDelayMs,
                                 m_targetBitrateUpdatedAtMs,
                                 static_cast<uint32_t>(m_config.bitrate),
                                 steadyNowMs());
            const auto sendStartPlan = VideoThreadLifecycleStateMachine::planSendThreadStart(
                startSendThread, m_threadRuntime.sendJoinable());
            if (sendStartPlan.shouldStartSendThread) {
                m_sendFrameRingBuffer.reset();
                m_threadRuntime.startCapture([this] { captureLoop(); },
                                             sendStartPlan.shouldJoinExistingSendThread);
                m_threadRuntime.startSend([this] { sendLoop(); },
                                          sendStartPlan.shouldJoinExistingSendThread);
            }
        }
        qInfo().noquote() << "[screen-session] sharing enabled localPort=" << localPort()
                          << "ssrc=" << videoSsrc();
        m_stateWaitCondition.wakeAll();
        return true;
    }

    if (!m_sharingEnabled.exchange(false, std::memory_order_acq_rel)) {
        return true;
    }
    if (m_cameraRelay) {
        m_cameraRelay->setSharingEnabled(false);
    }

    const auto disablePlan = VideoSessionStateMachine::planDisableSharing(
        true,
        m_cameraSendingEnabled.load(std::memory_order_acquire));
    const auto sendStopPlan = VideoThreadLifecycleStateMachine::planSendThreadStop(
        disablePlan.shouldJoinSendThread, m_threadRuntime.sendJoinable());
    if (sendStopPlan.shouldJoinSendThread) {
        m_sendFrameRingBuffer.close();
        m_threadRuntime.joinCapture();
        m_threadRuntime.joinSend();
    } else if (m_threadRuntime.captureJoinable()) {
        m_sendFrameRingBuffer.close();
        m_threadRuntime.joinCapture();
    }
    if (disablePlan.shouldSetForceKeyFrame) {
        m_forceKeyFramePending.store(disablePlan.forceKeyFrameValue, std::memory_order_release);
    }

    {
        QMutexLocker locker(&m_mutex);
        stopCaptureLocked();
    }
    m_stateWaitCondition.wakeAll();
    return true;
}

bool ScreenShareSession::sharingEnabled() const {
    return m_sharingEnabled.load(std::memory_order_acquire);
}

void ScreenShareSession::setExpectedRemoteVideoSsrc(uint32_t ssrc) {
    m_expectedRemoteVideoSsrc.store(ssrc, std::memory_order_release);
}

uint32_t ScreenShareSession::expectedRemoteVideoSsrc() const {
    return m_expectedRemoteVideoSsrc.load(std::memory_order_acquire);
}

void ScreenShareSession::setPeer(const std::string& address, uint16_t port) {
    QMutexLocker locker(&m_mutex);
    const bool hadPeerValid = m_mediaSocket.hasPeer();
    const media::UdpEndpoint previousPeer = hadPeerValid ? m_mediaSocket.peer() : media::UdpEndpoint{};
    if (hadPeerValid && m_config.peerAddress == address && m_config.peerPort == port) {
        return;
    }
    m_config.peerAddress = address;
    m_config.peerPort = port;
    const auto statusCallback = m_statusCallback;
    if (m_mediaSocket.isOpen()) {
        if (m_mediaSocket.setPeer(m_config.peerAddress, m_config.peerPort)) {
            const bool peerChanged = !hadPeerValid ||
                                     !media::UdpPeerSocket::sameIpv4Endpoint(previousPeer, m_mediaSocket.peer(), true);
            if (peerChanged) {
                qInfo().noquote() << "[screen-session] peer=" << QString::fromStdString(address) << ":" << port;
                if (statusCallback) {
                    statusCallback("Video peer configured");
                }
            }
        }
    }
    m_stateWaitCondition.wakeAll();
}

void ScreenShareSession::setDecodedFrameCallback(std::function<void(av::codec::DecodedVideoFrame)> callback) {
    QMutexLocker locker(&m_mutex);
    m_decodedFrameCallback = std::move(callback);
}

void ScreenShareSession::setDecodedFrameWithSsrcCallback(
    std::function<void(av::codec::DecodedVideoFrame, uint32_t)> callback) {
    QMutexLocker locker(&m_mutex);
    m_decodedFrameWithSsrcCallback = std::move(callback);
}

void ScreenShareSession::setErrorCallback(std::function<void(std::string)> callback) {
    QMutexLocker locker(&m_mutex);
    m_errorCallback = std::move(callback);
}

void ScreenShareSession::setCameraSourceCallback(std::function<void(bool syntheticFallback)> callback) {
    QMutexLocker locker(&m_mutex);
    m_cameraSourceCallback = std::move(callback);
}

void ScreenShareSession::setStatusCallback(std::function<void(std::string)> callback) {
    QMutexLocker locker(&m_mutex);
    m_statusCallback = std::move(callback);
}

uint16_t ScreenShareSession::localPort() const {
    QMutexLocker locker(&m_mutex);
    return m_mediaSocket.localPort();
}

uint32_t ScreenShareSession::videoSsrc() const {
    return m_sender.ssrc();
}

bool ScreenShareSession::isRunning() const {
    return m_running.load(std::memory_order_acquire);
}

std::string ScreenShareSession::lastError() const {
    QMutexLocker locker(&m_mutex);
    return m_lastError;
}

uint64_t ScreenShareSession::sentPacketCount() const {
    return m_sentPacketCount.load(std::memory_order_acquire);
}

uint64_t ScreenShareSession::receivedPacketCount() const {
    return m_receivedPacketCount.load(std::memory_order_acquire);
}

uint64_t ScreenShareSession::keyframeRequestCount() const {
    return m_keyframeRequestCount.load(std::memory_order_acquire);
}

uint64_t ScreenShareSession::retransmitPacketCount() const {
    return m_retransmitPacketCount.load(std::memory_order_acquire);
}

uint64_t ScreenShareSession::bitrateReconfigureCount() const {
    return m_bitrateReconfigureCount.load(std::memory_order_acquire);
}

uint32_t ScreenShareSession::lastBitrateApplyDelayMs() const {
    return m_lastBitrateApplyDelayMs.load(std::memory_order_acquire);
}

uint32_t ScreenShareSession::targetBitrateBps() const {
    return m_targetBitrateBps.load(std::memory_order_acquire);
}

uint32_t ScreenShareSession::appliedBitrateBps() const {
    return m_appliedBitrateBps.load(std::memory_order_acquire);
}

}  // namespace av::session
