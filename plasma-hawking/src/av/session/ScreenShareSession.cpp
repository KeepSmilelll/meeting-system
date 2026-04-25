#include "ScreenShareSession.h"

#include "VideoSessionControlActions.h"
#include "VideoSessionStateMachine.h"
#include "VideoThreadLifecycleStateMachine.h"

#include "net/media/SocketAddressUtils.h"

#include <QDebug>
#include <QByteArray>
#include <QList>
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

uint32_t chooseConfiguredOrRandomSsrc(uint32_t configuredSsrc) {
    return configuredSsrc != 0U ? configuredSsrc : makeSsrc();
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
    m_dtlsStarted.store(false, std::memory_order_release);
    m_iceConnected.store(false, std::memory_order_release);
    m_dtlsConnected.store(false, std::memory_order_release);
    m_srtpReady.store(false, std::memory_order_release);
    m_inboundSrtp.clear();
    m_outboundSrtp.clear();
    m_dtlsHandshakePackets.clear();
    m_lastDtlsHandshakeSendAt = {};
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
                resetSenderForFreshStream(m_sender,
                                          m_rtcpActionPipeline,
                                          chooseConfiguredOrRandomSsrc(
                                              m_configuredVideoSsrc.load(std::memory_order_acquire)));
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
    m_mediaSocket.disableTurnRelay();
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

bool ScreenShareSession::configureTurnRelay(const std::string& turnAddress,
                                            uint16_t turnPort,
                                            const std::string& username,
                                            const std::string& credential,
                                            const std::string& peerAddress,
                                            uint16_t peerPort) {
    QMutexLocker locker(&m_mutex);
    if (!m_mediaSocket.isOpen()) {
        setErrorLocked("video TURN relay skipped: socket not open");
        return false;
    }
    m_config.peerAddress = peerAddress;
    m_config.peerPort = peerPort;
    std::string error;
    if (!m_mediaSocket.configureTurnRelay(turnAddress,
                                          turnPort,
                                          username,
                                          credential,
                                          peerAddress,
                                          peerPort,
                                          &error)) {
        setErrorLocked(error.empty() ? "video TURN relay configure failed" : error);
        return false;
    }
    if (m_statusCallback) {
        m_statusCallback(QStringLiteral("Video TURN relay configured: %1:%2")
                             .arg(QString::fromStdString(turnAddress))
                             .arg(turnPort)
                             .toStdString());
    }
    return true;
}

void ScreenShareSession::setVideoSsrc(uint32_t ssrc) {
    if (ssrc == 0) {
        return;
    }
    m_configuredVideoSsrc.store(ssrc, std::memory_order_release);
    QMutexLocker locker(&m_mutex);
    m_sender.setSSRC(ssrc);
}

QString ScreenShareSession::prepareDtlsFingerprint() {
    QMutexLocker locker(&m_mutex);
    if (!m_dtlsTransport.prepareLocalFingerprint()) {
        setErrorLocked(m_dtlsTransport.lastError().toStdString());
        return {};
    }
    return m_dtlsTransport.localFingerprintSha256();
}

bool ScreenShareSession::startDtlsSrtp(const QString& serverFingerprint) {
    QMutexLocker locker(&m_mutex);
    if (serverFingerprint.trimmed().isEmpty() || !m_mediaSocket.isOpen() || !m_mediaSocket.hasPeer()) {
        if (m_statusCallback) {
            m_statusCallback("Video dtls-start skipped: missing fingerprint or peer");
        }
        return false;
    }
    if (m_dtlsTransport.isConnected() && m_srtpReady.load(std::memory_order_acquire)) {
        return true;
    }

    QList<QByteArray> outgoing;
    if (!m_dtlsTransport.start(serverFingerprint, &outgoing)) {
        setErrorLocked(m_dtlsTransport.lastError().toStdString());
        return false;
    }

    if (!outgoing.empty() || !m_dtlsStarted.load(std::memory_order_acquire)) {
        m_dtlsHandshakePackets.assign(outgoing.begin(), outgoing.end());
    }
    m_dtlsStarted.store(true, std::memory_order_release);
    if (m_statusCallback) {
        m_statusCallback(QStringLiteral("Video dtls-started cached_packets=%1")
                             .arg(static_cast<int>(m_dtlsHandshakePackets.size()))
                             .toStdString());
    }
    return sendCachedDtlsHandshakeLocked(m_mediaSocket.peer());
}

bool ScreenShareSession::iceConnected() const {
    return m_iceConnected.load(std::memory_order_acquire);
}

bool ScreenShareSession::dtlsConnected() const {
    return m_dtlsConnected.load(std::memory_order_acquire);
}

bool ScreenShareSession::srtpReady() const {
    return m_srtpReady.load(std::memory_order_acquire);
}

bool ScreenShareSession::sendTransportProbe(const std::vector<uint8_t>& packet) {
    if (packet.empty()) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    if (!m_mediaSocket.isOpen() || !m_mediaSocket.hasPeer()) {
        return false;
    }
    const media::UdpEndpoint peer = m_mediaSocket.peer();
    const int sent = m_mediaSocket.sendTo(packet.data(), packet.size(), peer);
    return sent == static_cast<int>(packet.size());
}

bool ScreenShareSession::configureSrtpLocked() {
    if (m_srtpReady.load(std::memory_order_acquire)) {
        return true;
    }
    if (!m_dtlsTransport.isConnected()) {
        return false;
    }

    media::DtlsTransportClient::SrtpKeyMaterial keying;
    if (!m_dtlsTransport.exportSrtpKeyMaterial(
            security::SRTPContext::masterKeyLength(),
            security::SRTPContext::masterSaltLength(),
            &keying)) {
        setErrorLocked("video DTLS SRTP exporter failed");
        return false;
    }
    if (!m_inboundSrtp.configure(keying.remoteKey, keying.remoteSalt, security::SRTPContext::Direction::Inbound) ||
        !m_outboundSrtp.configure(keying.localKey, keying.localSalt, security::SRTPContext::Direction::Outbound)) {
        const QString error = !m_inboundSrtp.lastError().isEmpty() ? m_inboundSrtp.lastError() : m_outboundSrtp.lastError();
        setErrorLocked(error.toStdString());
        return false;
    }
    m_dtlsConnected.store(true, std::memory_order_release);
    m_srtpReady.store(true, std::memory_order_release);
    m_forceKeyFramePending.store(true, std::memory_order_release);
    qInfo().noquote() << "[screen-session] dtls-connected srtp-ready profile=" << m_dtlsTransport.selectedSrtpProfile();
    if (m_statusCallback) {
        m_statusCallback(QStringLiteral("Video srtp-ready profile=%1")
                             .arg(m_dtlsTransport.selectedSrtpProfile())
                             .toStdString());
    }
    return true;
}

bool ScreenShareSession::handleDtlsPacketLocked(const uint8_t* data, std::size_t len, const media::UdpEndpoint& from) {
    if (data == nullptr || len == 0U || !m_mediaSocket.acceptSender(from)) {
        if (m_statusCallback) {
            m_statusCallback("Video dtls-packet rejected: unexpected sender");
        }
        return false;
    }

    if (m_statusCallback) {
        m_statusCallback(QStringLiteral("Video dtls-packet received bytes=%1")
                             .arg(static_cast<qulonglong>(len))
                             .toStdString());
    }
    QList<QByteArray> outgoing;
    const QByteArray datagram(reinterpret_cast<const char*>(data), static_cast<int>(len));
    if (!m_dtlsTransport.handleIncomingDatagram(datagram, &outgoing)) {
        setErrorLocked(m_dtlsTransport.lastError().toStdString());
        return false;
    }
    for (const QByteArray& packet : outgoing) {
        const int sent = m_mediaSocket.sendTo(
            reinterpret_cast<const uint8_t*>(packet.constData()),
            static_cast<std::size_t>(packet.size()),
            from);
        if (sent != packet.size()) {
            setErrorLocked("video DTLS response send failed");
            return false;
        }
    }
    return configureSrtpLocked();
}

bool ScreenShareSession::sendCachedDtlsHandshakeLocked(const media::UdpEndpoint& peer) {
    if (m_srtpReady.load(std::memory_order_acquire) || !peer.isValid() || m_dtlsHandshakePackets.empty()) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_lastDtlsHandshakeSendAt.time_since_epoch().count() != 0 &&
        now - m_lastDtlsHandshakeSendAt < std::chrono::milliseconds(100)) {
        return true;
    }
    for (const QByteArray& packet : m_dtlsHandshakePackets) {
        const int sent = m_mediaSocket.sendTo(
            reinterpret_cast<const uint8_t*>(packet.constData()),
            static_cast<std::size_t>(packet.size()),
            peer);
        if (sent != packet.size()) {
            setErrorLocked("video DTLS handshake send failed");
            return false;
        }
    }
    m_lastDtlsHandshakeSendAt = now;
    if (m_statusCallback) {
        m_statusCallback(QStringLiteral("Video dtls-handshake sent packets=%1")
                             .arg(static_cast<int>(m_dtlsHandshakePackets.size()))
                             .toStdString());
    }
    return true;
}

bool ScreenShareSession::protectRtpLocked(std::vector<uint8_t>* packet) {
    if (!m_dtlsStarted.load(std::memory_order_acquire)) {
        return true;
    }
    if (!m_srtpReady.load(std::memory_order_acquire)) {
        return false;
    }
    QByteArray bytes(reinterpret_cast<const char*>(packet->data()), static_cast<int>(packet->size()));
    if (!m_outboundSrtp.protectRtp(&bytes)) {
        setErrorLocked(m_outboundSrtp.lastError().toStdString());
        return false;
    }
    packet->assign(reinterpret_cast<const uint8_t*>(bytes.constData()),
                   reinterpret_cast<const uint8_t*>(bytes.constData()) + bytes.size());
    return true;
}

bool ScreenShareSession::protectRtcpLocked(std::vector<uint8_t>* packet) {
    if (!m_dtlsStarted.load(std::memory_order_acquire)) {
        return true;
    }
    if (!m_srtpReady.load(std::memory_order_acquire)) {
        return false;
    }
    QByteArray bytes(reinterpret_cast<const char*>(packet->data()), static_cast<int>(packet->size()));
    if (!m_outboundSrtp.protectRtcp(&bytes)) {
        setErrorLocked(m_outboundSrtp.lastError().toStdString());
        return false;
    }
    packet->assign(reinterpret_cast<const uint8_t*>(bytes.constData()),
                   reinterpret_cast<const uint8_t*>(bytes.constData()) + bytes.size());
    return true;
}

bool ScreenShareSession::unprotectRtpLocked(std::vector<uint8_t>* packet) {
    if (!m_dtlsStarted.load(std::memory_order_acquire)) {
        return true;
    }
    if (!m_srtpReady.load(std::memory_order_acquire)) {
        return false;
    }
    QByteArray bytes(reinterpret_cast<const char*>(packet->data()), static_cast<int>(packet->size()));
    if (!m_inboundSrtp.unprotectRtp(&bytes)) {
        const uint8_t first = packet->empty() ? 0U : (*packet)[0];
        const uint8_t second = packet->size() > 1U ? (*packet)[1] : 0U;
        setErrorLocked(QStringLiteral("%1 rtp_len=%2 first=%3 second=%4")
                           .arg(m_inboundSrtp.lastError())
                           .arg(static_cast<qulonglong>(packet->size()))
                           .arg(static_cast<unsigned int>(first))
                           .arg(static_cast<unsigned int>(second))
                           .toStdString());
        return false;
    }
    packet->assign(reinterpret_cast<const uint8_t*>(bytes.constData()),
                   reinterpret_cast<const uint8_t*>(bytes.constData()) + bytes.size());
    return true;
}

bool ScreenShareSession::unprotectRtcpLocked(std::vector<uint8_t>* packet) {
    if (!m_dtlsStarted.load(std::memory_order_acquire)) {
        return true;
    }
    if (!m_srtpReady.load(std::memory_order_acquire)) {
        return false;
    }
    QByteArray bytes(reinterpret_cast<const char*>(packet->data()), static_cast<int>(packet->size()));
    if (!m_inboundSrtp.unprotectRtcp(&bytes)) {
        const uint8_t first = packet->empty() ? 0U : (*packet)[0];
        const uint8_t second = packet->size() > 1U ? (*packet)[1] : 0U;
        setErrorLocked(QStringLiteral("%1 rtcp_len=%2 first=%3 second=%4")
                           .arg(m_inboundSrtp.lastError())
                           .arg(static_cast<qulonglong>(packet->size()))
                           .arg(static_cast<unsigned int>(first))
                           .arg(static_cast<unsigned int>(second))
                           .toStdString());
        return false;
    }
    packet->assign(reinterpret_cast<const uint8_t*>(bytes.constData()),
                   reinterpret_cast<const uint8_t*>(bytes.constData()) + bytes.size());
    return true;
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

void ScreenShareSession::setLocalCameraPreviewCallback(std::function<void(av::codec::DecodedVideoFrame)> callback) {
    QMutexLocker locker(&m_mutex);
    m_localCameraPreviewCallback = std::move(callback);
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
