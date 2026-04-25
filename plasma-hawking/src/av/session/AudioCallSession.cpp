#include "AudioCallSession.h"

#include "net/media/SocketAddressUtils.h"

#include <QDebug>
#include <QCoreApplication>
#include <QByteArray>
#include <QList>
#include <QMutexLocker>
#include <QThread>
#include <QtGlobal>

#include <algorithm>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool audioProcessEnabled(const char* key) {
    if (!qEnvironmentVariableIsSet(key)) {
        return true;
    }
    return qEnvironmentVariableIntValue(key) != 0;
}

float audioProcessFloat(const char* key, float fallback) {
    bool ok = false;
    const float value = qEnvironmentVariable(key).toFloat(&ok);
    return ok ? value : fallback;
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

}  // namespace

av::session::AudioCallSession::AudioCallSession(AudioCallSessionConfig config)
    : m_config(std::move(config)),
      m_capture(32),
      m_jitter(m_config.jitterPackets == 0 ? 32 : m_config.jitterPackets,
               3,
               std::chrono::milliseconds(20)),
      m_player(32) {
    if (m_config.frameSamples <= 0) {
        m_config.frameSamples = 960;
    }
    if (m_config.channels != 1 && m_config.channels != 2) {
        m_config.channels = 1;
    }
}

av::session::AudioCallSession::~AudioCallSession() {
    stop();
}

bool av::session::AudioCallSession::start() {
    if (m_running.load(std::memory_order_acquire) || m_sendThread != nullptr || m_recvThread != nullptr) {
        return false;
    }

    if (!m_encoder.configure(m_config.sampleRate, m_config.channels, m_config.bitrate)) {
        setErrorLocked("encoder configure failed");
        return false;
    }

    const int captureFrameSamples = m_config.frameSamples > 0 ? m_config.frameSamples : 960;
    const int encoderFrameSamples = m_encoder.frameSamples() > 0 ? m_encoder.frameSamples() : captureFrameSamples;
    m_config.frameSamples = captureFrameSamples;

    if (!m_decoder.configure(m_config.sampleRate, m_config.channels) ||
        !m_player.configure(m_config.sampleRate, m_config.channels, captureFrameSamples)) {
        setErrorLocked("decoder/player configure failed");
        return false;
    }

    av::process::AcousticEchoCanceller::Config aecConfig{};
    aecConfig.sampleRate = m_config.sampleRate;
    aecConfig.channels = m_config.channels;
    aecConfig.frameSamples = captureFrameSamples;
    aecConfig.renderHistoryMs = (std::max)(200, qEnvironmentVariableIntValue("MEETING_AUDIO_AEC_HISTORY_MS"));
    aecConfig.suppression = std::clamp(audioProcessFloat("MEETING_AUDIO_AEC_SUPPRESSION", 0.85F), 0.0F, 1.0F);
    if (!m_aec.configure(aecConfig)) {
        setErrorLocked("aec configure failed");
        return false;
    }
    m_aec.setEnabled(audioProcessEnabled("MEETING_AUDIO_AEC"));

    av::process::NoiseSuppressor::Config nsConfig{};
    nsConfig.sampleRate = m_config.sampleRate;
    nsConfig.channels = m_config.channels;
    nsConfig.maxSuppressionDb = std::clamp(audioProcessFloat("MEETING_AUDIO_NS_MAX_DB", 18.0F), 0.0F, 36.0F);
    nsConfig.floorGain = std::clamp(audioProcessFloat("MEETING_AUDIO_NS_FLOOR_GAIN", 0.18F), 0.05F, 1.0F);
    if (!m_noiseSuppressor.configure(nsConfig)) {
        setErrorLocked("ns configure failed");
        return false;
    }
    m_noiseSuppressor.setEnabled(audioProcessEnabled("MEETING_AUDIO_NS"));

    av::process::AutoGainControl::Config agcConfig{};
    agcConfig.sampleRate = m_config.sampleRate;
    agcConfig.channels = m_config.channels;
    agcConfig.targetRms = std::clamp(audioProcessFloat("MEETING_AUDIO_AGC_TARGET_RMS", 0.12F), 0.03F, 0.35F);
    agcConfig.minGain = std::clamp(audioProcessFloat("MEETING_AUDIO_AGC_MIN_GAIN", 0.25F), 0.1F, 1.0F);
    agcConfig.maxGain = std::clamp(audioProcessFloat("MEETING_AUDIO_AGC_MAX_GAIN", 8.0F), 1.0F, 12.0F);
    if (!m_autoGainControl.configure(agcConfig)) {
        setErrorLocked("agc configure failed");
        return false;
    }
    m_autoGainControl.setEnabled(audioProcessEnabled("MEETING_AUDIO_AGC"));

    m_playoutAssembler = {};
    m_jitter.clear();
    m_sender.setSequence(0);
    m_sender.setSSRC(static_cast<uint32_t>(std::random_device{}()));
    m_lastRttMs.store(0, std::memory_order_release);
    m_targetBitrateBps.store(static_cast<uint32_t>(m_config.bitrate), std::memory_order_release);
    m_lastReceivedSequence.store(0, std::memory_order_release);
    m_receivedPacketCount.store(0, std::memory_order_release);
    m_sentPacketCount.store(0, std::memory_order_release);
    m_sentOctetCount.store(0, std::memory_order_release);
    m_dtlsStarted.store(false, std::memory_order_release);
    m_iceConnected.store(false, std::memory_order_release);
    m_dtlsConnected.store(false, std::memory_order_release);
    m_srtpReady.store(false, std::memory_order_release);
    m_inboundSrtp.clear();
    m_outboundSrtp.clear();
    m_dtlsHandshakePackets.clear();
    m_lastDtlsHandshakeSendAt = {};
    m_audioBwe.reset();
    m_localSenderReport = {};
    m_remoteSenderReport = {};
    m_aec.reset();
    m_noiseSuppressor.reset();
    m_autoGainControl.reset();

    if (!m_capture.start()) {
        setErrorLocked("audio capture start failed");
        m_capture.stop();
        m_player.stop();
        return false;
    }
    if (!m_player.start()) {
        setErrorLocked("audio player start failed");
        m_capture.stop();
        m_player.stop();
        return false;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_lastError.clear();
        if (!openSocketLocked()) {
            m_capture.stop();
            m_player.stop();
            return false;
        }
    }

    m_running.store(true, std::memory_order_release);
    m_stateWaitCondition.wakeAll();
    qInfo().noquote() << "[audio-session] encoderFrameSamples=" << encoderFrameSamples << "playoutFrameSamples=" << captureFrameSamples << "bitrate=" << m_config.bitrate;
    qInfo().noquote() << "[audio-session] started localPort=" << localPort();
    m_sendThread = QThread::create([this]() { sendLoop(); });
    m_recvThread = QThread::create([this]() { recvLoop(); });
    if (m_sendThread == nullptr || m_recvThread == nullptr) {
        m_running.store(false, std::memory_order_release);
        if (m_sendThread != nullptr) {
            delete m_sendThread;
            m_sendThread = nullptr;
        }
        if (m_recvThread != nullptr) {
            delete m_recvThread;
            m_recvThread = nullptr;
        }
        {
            QMutexLocker locker(&m_mutex);
            setErrorLocked("audio worker thread alloc failed");
            closeSocketLocked();
        }
        m_capture.stop();
        m_player.stop();
        return false;
    }
    m_sendThread->start();
    m_recvThread->start();
    return true;
}

void av::session::AudioCallSession::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel) && m_sendThread == nullptr && m_recvThread == nullptr) {
        return;
    }
    m_stateWaitCondition.wakeAll();
    m_mediaSocket.interruptWaiters();

    m_capture.stop();
    m_player.stop();

    auto stopThread = [](QThread*& thread) {
        if (thread == nullptr) {
            return;
        }
        thread->wait();
        delete thread;
        thread = nullptr;
    };
    stopThread(m_sendThread);
    stopThread(m_recvThread);

    {
        QMutexLocker locker(&m_mutex);
        closeSocketLocked();
    }
}

bool av::session::AudioCallSession::submitLocalFrame(av::capture::AudioFrame frame) {
    if (frame.sampleRate != m_config.sampleRate) {
        frame.sampleRate = m_config.sampleRate;
    }
    if (frame.channels != m_config.channels) {
        frame.channels = m_config.channels;
    }
    if (frame.samples.size() != static_cast<std::size_t>(m_config.frameSamples) * static_cast<std::size_t>(m_config.channels)) {
        return false;
    }
    return m_capture.pushCapturedFrame(std::move(frame));
}

bool av::session::AudioCallSession::waitForPlayedFrame(av::capture::AudioFrame& outFrame, std::chrono::milliseconds timeout) {
    return m_player.waitForPlayedFrame(outFrame, timeout);
}

void av::session::AudioCallSession::setCaptureMuted(bool muted) {
    m_captureMuted.store(muted, std::memory_order_release);
}

bool av::session::AudioCallSession::captureMuted() const {
    return m_captureMuted.load(std::memory_order_acquire);
}
bool av::session::AudioCallSession::setPreferredInputDeviceName(const QString& deviceName) {
    return m_capture.setPreferredDeviceName(deviceName);
}

bool av::session::AudioCallSession::setPreferredOutputDeviceName(const QString& deviceName) {
    return m_player.setPreferredOutputDeviceName(deviceName);
}

QString av::session::AudioCallSession::preferredInputDeviceName() const {
    return m_capture.preferredDeviceName();
}

QString av::session::AudioCallSession::preferredOutputDeviceName() const {
    return m_player.preferredOutputDeviceName();
}

void av::session::AudioCallSession::setPeer(const std::string& address, uint16_t port) {
    QMutexLocker locker(&m_mutex);
    m_config.peerAddress = address;
    m_config.peerPort = port;
    m_mediaSocket.disableTurnRelay();
    if (m_mediaSocket.isOpen() && m_mediaSocket.setPeer(m_config.peerAddress, m_config.peerPort)) {
        qInfo().noquote() << "[audio-session] peer=" << QString::fromStdString(address) << ":" << port;
    }
    m_stateWaitCondition.wakeAll();
}

bool av::session::AudioCallSession::configureTurnRelay(const std::string& turnAddress,
                                                       uint16_t turnPort,
                                                       const std::string& username,
                                                       const std::string& credential,
                                                       const std::string& peerAddress,
                                                       uint16_t peerPort) {
    QMutexLocker locker(&m_mutex);
    if (!m_mediaSocket.isOpen()) {
        setErrorLocked("audio TURN relay skipped: socket not open");
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
        setErrorLocked(error.empty() ? "audio TURN relay configure failed" : error);
        return false;
    }
    qInfo().noquote() << "[audio-session] turn-relay=" << QString::fromStdString(turnAddress) << ":" << turnPort;
    return true;
}

void av::session::AudioCallSession::setAudioSsrc(uint32_t ssrc) {
    if (ssrc == 0) {
        return;
    }
    m_sender.setSSRC(ssrc);
}

QString av::session::AudioCallSession::prepareDtlsFingerprint() {
    QMutexLocker locker(&m_mutex);
    if (!m_dtlsTransport.prepareLocalFingerprint()) {
        setErrorLocked(m_dtlsTransport.lastError().toStdString());
        return {};
    }
    return m_dtlsTransport.localFingerprintSha256();
}

bool av::session::AudioCallSession::startDtlsSrtp(const QString& serverFingerprint) {
    QMutexLocker locker(&m_mutex);
    if (serverFingerprint.trimmed().isEmpty() || !m_mediaSocket.isOpen() || !m_mediaSocket.hasPeer()) {
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
    return sendCachedDtlsHandshakeLocked(m_mediaSocket.peer());
}

bool av::session::AudioCallSession::iceConnected() const {
    return m_iceConnected.load(std::memory_order_acquire);
}

bool av::session::AudioCallSession::dtlsConnected() const {
    return m_dtlsConnected.load(std::memory_order_acquire);
}

bool av::session::AudioCallSession::srtpReady() const {
    return m_srtpReady.load(std::memory_order_acquire);
}

bool av::session::AudioCallSession::sendTransportProbe(const std::vector<uint8_t>& packet) {
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

bool av::session::AudioCallSession::configureSrtpLocked() {
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
        setErrorLocked("DTLS SRTP exporter failed");
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
    qInfo().noquote() << "[audio-session] dtls-connected srtp-ready profile=" << m_dtlsTransport.selectedSrtpProfile();
    return true;
}

bool av::session::AudioCallSession::handleDtlsPacketLocked(const uint8_t* data, std::size_t len, const media::UdpEndpoint& from) {
    if (data == nullptr || len == 0U || !m_mediaSocket.acceptSender(from)) {
        return false;
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
            setErrorLocked("DTLS response send failed");
            return false;
        }
    }
    return configureSrtpLocked();
}

bool av::session::AudioCallSession::sendCachedDtlsHandshakeLocked(const media::UdpEndpoint& peer) {
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
            setErrorLocked("DTLS handshake send failed");
            return false;
        }
    }
    m_lastDtlsHandshakeSendAt = now;
    return true;
}

bool av::session::AudioCallSession::protectRtpLocked(std::vector<uint8_t>* packet) {
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

bool av::session::AudioCallSession::protectRtcpLocked(std::vector<uint8_t>* packet) {
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

bool av::session::AudioCallSession::unprotectRtpLocked(std::vector<uint8_t>* packet) {
    if (!m_dtlsStarted.load(std::memory_order_acquire)) {
        return true;
    }
    if (!m_srtpReady.load(std::memory_order_acquire)) {
        return false;
    }
    QByteArray bytes(reinterpret_cast<const char*>(packet->data()), static_cast<int>(packet->size()));
    if (!m_inboundSrtp.unprotectRtp(&bytes)) {
        setErrorLocked(m_inboundSrtp.lastError().toStdString());
        return false;
    }
    packet->assign(reinterpret_cast<const uint8_t*>(bytes.constData()),
                   reinterpret_cast<const uint8_t*>(bytes.constData()) + bytes.size());
    return true;
}

bool av::session::AudioCallSession::unprotectRtcpLocked(std::vector<uint8_t>* packet) {
    if (!m_dtlsStarted.load(std::memory_order_acquire)) {
        return true;
    }
    if (!m_srtpReady.load(std::memory_order_acquire)) {
        return false;
    }
    QByteArray bytes(reinterpret_cast<const char*>(packet->data()), static_cast<int>(packet->size()));
    if (!m_inboundSrtp.unprotectRtcp(&bytes)) {
        setErrorLocked(m_inboundSrtp.lastError().toStdString());
        return false;
    }
    packet->assign(reinterpret_cast<const uint8_t*>(bytes.constData()),
                   reinterpret_cast<const uint8_t*>(bytes.constData()) + bytes.size());
    return true;
}

uint16_t av::session::AudioCallSession::localPort() const {
    QMutexLocker locker(&m_mutex);
    return m_mediaSocket.localPort();
}

uint32_t av::session::AudioCallSession::audioSsrc() const {
    return m_sender.ssrc();
}

uint32_t av::session::AudioCallSession::lastRttMs() const {
    return m_lastRttMs.load(std::memory_order_acquire);
}

uint32_t av::session::AudioCallSession::targetBitrateBps() const {
    return m_targetBitrateBps.load(std::memory_order_acquire);
}

uint64_t av::session::AudioCallSession::sentPacketCount() const {
    return static_cast<uint64_t>(m_sentPacketCount.load(std::memory_order_acquire));
}

uint64_t av::session::AudioCallSession::receivedPacketCount() const {
    return static_cast<uint64_t>(m_receivedPacketCount.load(std::memory_order_acquire));
}

uint64_t av::session::AudioCallSession::playedFrameCount() const {
    return m_player.stats().playedFrames;
}

bool av::session::AudioCallSession::isRunning() const {
    return m_running.load(std::memory_order_acquire);
}

std::string av::session::AudioCallSession::lastError() const {
    QMutexLocker locker(&m_mutex);
    return m_lastError;
}

av::render::AudioPlayer& av::session::AudioCallSession::player() {
    return m_player;
}

const av::render::AudioPlayer& av::session::AudioCallSession::player() const {
    return m_player;
}

std::shared_ptr<av::sync::AVSync> av::session::AudioCallSession::clock() const {
    return m_player.clock();
}

bool av::session::AudioCallSession::openSocketLocked() {
    std::string socketError;
    if (!m_mediaSocket.open(m_config.localAddress, m_config.localPort, &socketError)) {
        setErrorLocked(socketError.empty() ? "socket setup failed" : socketError);
        closeSocketLocked();
        return false;
    }

    (void)m_mediaSocket.setPeer(m_config.peerAddress, m_config.peerPort);
    m_mediaSocket.setReadTimeoutMs(50);
    return true;
}

void av::session::AudioCallSession::closeSocketLocked() {
    m_mediaSocket.close();
}

uint64_t av::session::AudioCallSession::currentNtpTimestamp() {
    constexpr uint64_t kUnixToNtpEpochOffset = 2208988800ULL;

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count();
    const uint64_t ntpSeconds = kUnixToNtpEpochOffset + static_cast<uint64_t>(seconds.count());
    const uint64_t ntpFraction = (static_cast<uint64_t>(nanoseconds) << 32U) / 1000000000ULL;
    return (ntpSeconds << 32U) | ntpFraction;
}

uint32_t av::session::AudioCallSession::compactNtpFromTimestamp(uint64_t ntpTimestamp) {
    return static_cast<uint32_t>((ntpTimestamp >> 16U) & 0xFFFFFFFFULL);
}

uint32_t av::session::AudioCallSession::compactNtpFromElapsed(std::chrono::steady_clock::duration elapsed) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (micros <= 0) {
        return 0;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(micros) * 65536ULL) / 1000000ULL);
}

uint32_t av::session::AudioCallSession::compactNtpToMs(uint32_t compactNtp) {
    if (compactNtp == 0) {
        return 0;
    }

    return static_cast<uint32_t>(((static_cast<uint64_t>(compactNtp) * 1000ULL) + 65535ULL) / 65536ULL);
}

bool av::session::AudioCallSession::encodeAndSend(av::codec::AudioEncoder& encoder, const av::capture::AudioFrame& frame, const media::UdpEndpoint& peer, av::codec::EncodedAudioFrame* sentFrame) {
    if (sentFrame != nullptr) {
        *sentFrame = {};
    }

    av::codec::EncodedAudioFrame encoded;
    std::string encodeError;
    if (!encoder.encode(frame, encoded, &encodeError)) {
        if (encodeError.find("Resource temporarily unavailable") != std::string::npos) {
            return true;
        }
        setErrorLocked("encode failed sr=" + std::to_string(frame.sampleRate) +
                       " ch=" + std::to_string(frame.channels) +
                       " samples=" + std::to_string(frame.samples.size()) +
                       " pts=" + std::to_string(frame.pts) +
                       " expected=" + std::to_string(encoder.frameSamples()) +
                       " encoder_ch=" + std::to_string(encoder.channels()) +
                       " encoder_sr=" + std::to_string(encoder.sampleRate()) +
                       (encodeError.empty() ? std::string{} : std::string{" details="} + encodeError));
        return false;
    }

    auto packet = m_sender.buildPacket(encoded.payloadType,
                                       true,
                                       static_cast<uint32_t>(encoded.pts),
                                       encoded.payload);
    if (packet.empty()) {
        setErrorLocked("RTP build failed");
        return false;
    }
    {
        QMutexLocker locker(&m_mutex);
        if (!protectRtpLocked(&packet)) {
            return false;
        }
    }

    const int sent = m_mediaSocket.sendTo(packet.data(), packet.size(), peer);
    if (sent != static_cast<int>(packet.size())) {
        setErrorLocked("sendto failed");
        return false;
    }
    m_sentPacketCount.fetch_add(1, std::memory_order_relaxed);
    m_sentOctetCount.fetch_add(static_cast<uint32_t>(encoded.payload.size()), std::memory_order_relaxed);
    m_audioBwe.onPacketSent(encoded.payload.size());
    if (sentFrame != nullptr) {
        *sentFrame = std::move(encoded);
    }
    return true;
}

bool av::session::AudioCallSession::sendSenderReportLocked(const media::UdpEndpoint& peer, uint32_t rtpTimestamp) {
    if (!m_mediaSocket.isOpen() || rtpTimestamp == 0 || m_sentPacketCount.load(std::memory_order_relaxed) == 0) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_localSenderReport.compactNtp != 0 &&
        now > m_localSenderReport.observedAt &&
        (now - m_localSenderReport.observedAt) < std::chrono::milliseconds(500)) {
        return false;
    }

    media::RTCPSenderReport report{};
    report.senderSsrc = m_sender.ssrc();
    report.ntpTimestamp = currentNtpTimestamp();
    report.rtpTimestamp = rtpTimestamp;
    report.packetCount = m_sentPacketCount.load(std::memory_order_relaxed);
    report.octetCount = m_sentOctetCount.load(std::memory_order_relaxed);

    auto packet = m_rtcpHandler.buildSenderReport(report);
    if (packet.empty()) {
        setErrorLocked("RTCP SR build failed");
        return false;
    }
    if (!protectRtcpLocked(&packet)) {
        return false;
    }

    const int sent = m_mediaSocket.sendTo(packet.data(), packet.size(), peer);
    if (sent != static_cast<int>(packet.size())) {
        setErrorLocked("sendto RTCP SR failed");
        return false;
    }

    m_localSenderReport = SenderReportLedger{
        report.senderSsrc,
        compactNtpFromTimestamp(report.ntpTimestamp),
        now,
    };
    return true;
}

bool av::session::AudioCallSession::sendReceiverReportLocked(const media::UdpEndpoint& peer,
                                                                    uint32_t remoteSsrc,
                                                                    uint32_t lastSenderReport,
                                                                    uint32_t delaySinceLastSenderReport) {
    if (!m_mediaSocket.isOpen() || remoteSsrc == 0 || lastSenderReport == 0) {
        return false;
    }

    media::RTCPReceiverReport report{};
    report.receiverSsrc = m_sender.ssrc();
    report.reportBlocks.push_back(media::RTCPReportBlock{
        remoteSsrc,
        0,
        0,
        m_lastReceivedSequence.load(std::memory_order_relaxed),
        0,
        lastSenderReport,
        delaySinceLastSenderReport,
    });

    auto packet = m_rtcpHandler.buildReceiverReport(report);
    if (packet.empty()) {
        setErrorLocked("RTCP RR build failed");
        return false;
    }
    if (!protectRtcpLocked(&packet)) {
        return false;
    }

    const int sent = m_mediaSocket.sendTo(packet.data(), packet.size(), peer);
    if (sent != static_cast<int>(packet.size())) {
        setErrorLocked("sendto RTCP RR failed");
        return false;
    }

    return true;
}

bool av::session::AudioCallSession::handleRtcpPacketLocked(const uint8_t* data, std::size_t len, const media::UdpEndpoint& from) {
    std::vector<media::RTCPPacketSlice> slices;
    if (!m_rtcpHandler.parseCompoundPacket(data, len, slices)) {
        return false;
    }

    bool handled = false;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& slice : slices) {
        if (m_rtcpHandler.isSenderReport(slice.header)) {
            media::RTCPSenderReport report{};
            if (!m_rtcpHandler.parseSenderReport(data + slice.offset, slice.size, report)) {
                continue;
            }

            m_remoteSenderReport = SenderReportLedger{
                report.senderSsrc,
                compactNtpFromTimestamp(report.ntpTimestamp),
                now,
            };
            const uint32_t dlsr = compactNtpFromElapsed(std::chrono::steady_clock::now() - m_remoteSenderReport.observedAt);
            handled = sendReceiverReportLocked(from, report.senderSsrc, m_remoteSenderReport.compactNtp, dlsr) || handled;
            continue;
        }

        if (m_rtcpHandler.isReceiverReport(slice.header)) {
            media::RTCPReceiverReport report{};
            if (!m_rtcpHandler.parseReceiverReport(data + slice.offset, slice.size, report)) {
                continue;
            }

            for (const auto& block : report.reportBlocks) {
                if (block.sourceSsrc != m_sender.ssrc() ||
                    block.lastSenderReport == 0 ||
                    m_localSenderReport.compactNtp == 0 ||
                    block.lastSenderReport != m_localSenderReport.compactNtp ||
                    now <= m_localSenderReport.observedAt) {
                    continue;
                }

                const uint32_t arrivalCompactNtp =
                    m_localSenderReport.compactNtp + compactNtpFromElapsed(now - m_localSenderReport.observedAt);
                const uint64_t reflectedDelay =
                    static_cast<uint64_t>(block.lastSenderReport) + static_cast<uint64_t>(block.delaySinceLastSenderReport);
                if (static_cast<uint64_t>(arrivalCompactNtp) <= reflectedDelay) {
                    continue;
                }

                const uint32_t rttCompact =
                    static_cast<uint32_t>(static_cast<uint64_t>(arrivalCompactNtp) - reflectedDelay);
                const uint32_t rttMs = compactNtpToMs(rttCompact);
                m_lastRttMs.store(rttMs, std::memory_order_release);
                m_audioBwe.onReceiverReport(block.fractionLost, rttMs);
                const uint32_t estimatedKbps = m_audioBwe.estimateBitrateKbps();
                const uint32_t estimatedBps = std::max<uint32_t>(16000U, std::min<uint32_t>(64000U, estimatedKbps * 1000U));
                m_targetBitrateBps.store(estimatedBps, std::memory_order_release);
                handled = true;
            }
        }
    }

    return handled;
}

void av::session::AudioCallSession::drainReceivedPackets() {
    media::RTPPacket packet;
    while (m_jitter.pop(packet)) {
        av::codec::EncodedAudioFrame encoded;
        encoded.sampleRate = m_config.sampleRate;
        encoded.channels = m_config.channels;
        encoded.pts = packet.header.timestamp;
        encoded.payloadType = packet.header.payloadType;
        encoded.payload = packet.payload;

        av::capture::AudioFrame decoded;
        std::string decodeError;
        if (!m_decoder.decode(encoded, decoded, &decodeError)) {
            if (decodeError.find("Resource temporarily unavailable") != std::string::npos) {
                continue;
            }
            setErrorLocked(decodeError.empty() ? "decode failed" : std::string{"decode failed: "} + decodeError);
            continue;
        }

        queueDecodedFrameForPlayback(std::move(decoded));
    }
}

void av::session::AudioCallSession::queueDecodedFrameForPlayback(av::capture::AudioFrame frame) {
    const int channels = m_config.channels > 0 ? m_config.channels : 1;
    const int targetFrameSamples = m_player.frameSamples() > 0 ? m_player.frameSamples() : 960;
    const std::size_t targetSamples = static_cast<std::size_t>(targetFrameSamples) * static_cast<std::size_t>(channels);

    if (frame.sampleRate != m_config.sampleRate || frame.channels != channels || frame.samples.empty()) {
        setErrorLocked("decoded frame metadata mismatch");
        return;
    }
    if (targetSamples == 0 || (frame.samples.size() % static_cast<std::size_t>(channels)) != 0) {
        setErrorLocked("decoded frame sample count mismatch");
        return;
    }

    m_aec.pushRenderFrame(frame);

    if (m_playoutAssembler.samples.empty()) {
        m_playoutAssembler.sampleRate = frame.sampleRate;
        m_playoutAssembler.channels = frame.channels;
        m_playoutAssembler.pts = frame.pts;
    } else if (m_playoutAssembler.sampleRate != frame.sampleRate || m_playoutAssembler.channels != frame.channels) {
        m_playoutAssembler = {};
        m_playoutAssembler.sampleRate = frame.sampleRate;
        m_playoutAssembler.channels = frame.channels;
        m_playoutAssembler.pts = frame.pts;
    }

    m_playoutAssembler.samples.insert(m_playoutAssembler.samples.end(), frame.samples.begin(), frame.samples.end());
    while (m_playoutAssembler.samples.size() >= targetSamples) {
        av::capture::AudioFrame playout;
        playout.sampleRate = m_playoutAssembler.sampleRate;
        playout.channels = m_playoutAssembler.channels;
        playout.pts = m_playoutAssembler.pts;
        playout.samples.assign(m_playoutAssembler.samples.begin(), m_playoutAssembler.samples.begin() + static_cast<std::ptrdiff_t>(targetSamples));
        if (!m_player.enqueueFrame(std::move(playout))) {
            setErrorLocked("player enqueue failed");
            return;
        }

        m_playoutAssembler.samples.erase(m_playoutAssembler.samples.begin(), m_playoutAssembler.samples.begin() + static_cast<std::ptrdiff_t>(targetSamples));
        m_playoutAssembler.pts += static_cast<int64_t>(targetFrameSamples);
    }

    if (m_playoutAssembler.samples.empty()) {
        m_playoutAssembler.sampleRate = frame.sampleRate;
        m_playoutAssembler.channels = frame.channels;
        m_playoutAssembler.pts = frame.pts + static_cast<int64_t>(frame.samples.size() / static_cast<std::size_t>(channels));
    }
}
void av::session::AudioCallSession::setErrorLocked(std::string message) {
    m_lastError = std::move(message);
    qWarning().noquote() << "[audio-session]" << QString::fromStdString(m_lastError);
}

void av::session::AudioCallSession::sendLoop() {
    bool loggedFirstSentFrame = false;
    av::codec::AudioEncoder encoder;
    if (!encoder.configure(m_config.sampleRate, m_config.channels, m_config.bitrate)) {
        QMutexLocker locker(&m_mutex);
        setErrorLocked("send encoder configure failed");
        return;
    }

    while (m_running.load(std::memory_order_acquire)) {
        media::UdpEndpoint peer{};
        {
            QMutexLocker locker(&m_mutex);
            while (m_running.load(std::memory_order_acquire) && !m_mediaSocket.hasPeer()) {
                m_stateWaitCondition.wait(&m_mutex, 50);
            }
            if (!m_running.load(std::memory_order_acquire)) {
                break;
            }
            peer = m_mediaSocket.peer();
        }

        av::capture::AudioFrame frame;
        if (!m_capture.popFrameForEncode(frame, std::chrono::milliseconds(50))) {
            continue;
        }
        if (m_captureMuted.load(std::memory_order_acquire)) {
            continue;
        }

        std::string processError;
        if (!m_aec.processCaptureFrame(frame, &processError)) {
            setErrorLocked(processError.empty() ? "aec process failed" : processError);
            continue;
        }
        if (!m_noiseSuppressor.processFrame(frame, &processError)) {
            setErrorLocked(processError.empty() ? "ns process failed" : processError);
            continue;
        }
        if (!m_autoGainControl.processFrame(frame, &processError)) {
            setErrorLocked(processError.empty() ? "agc process failed" : processError);
            continue;
        }

        const uint32_t targetBitrate = m_targetBitrateBps.load(std::memory_order_acquire);
        if (targetBitrate >= 16000U && static_cast<int>(targetBitrate) != encoder.bitrate()) {
            if (!encoder.setBitrate(static_cast<int>(targetBitrate))) {
                setErrorLocked("audio encoder bitrate update failed");
                continue;
            }
        }

        const int encoderFrameSamples = encoder.frameSamples() > 0 ? encoder.frameSamples() : m_config.frameSamples;
        const std::size_t chunkSamples = static_cast<std::size_t>(encoderFrameSamples) * static_cast<std::size_t>(m_config.channels);
        if (encoderFrameSamples <= 0 || chunkSamples == 0 || (frame.samples.size() % chunkSamples) != 0) {
            setErrorLocked("capture frame not divisible by encoder frame size");
            continue;
        }

        bool sendFailed = false;
        const std::size_t chunkCount = frame.samples.size() / chunkSamples;
        for (std::size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            {
                QMutexLocker locker(&m_mutex);
                if (m_dtlsStarted.load(std::memory_order_acquire) &&
                    !m_srtpReady.load(std::memory_order_acquire)) {
                    (void)sendCachedDtlsHandshakeLocked(peer);
                    sendFailed = true;
                    break;
                }
            }

            av::capture::AudioFrame chunk;
            chunk.sampleRate = frame.sampleRate;
            chunk.channels = frame.channels;
            chunk.pts = frame.pts + static_cast<int64_t>(chunkIndex) * static_cast<int64_t>(encoderFrameSamples);
            const auto begin = frame.samples.begin() + static_cast<std::ptrdiff_t>(chunkIndex * chunkSamples);
            const auto end = begin + static_cast<std::ptrdiff_t>(chunkSamples);
            chunk.samples.assign(begin, end);

            av::codec::EncodedAudioFrame sentFrame;
            if (!encodeAndSend(encoder, chunk, peer, &sentFrame)) {
                sendFailed = true;
                break;
            }
            {
                QMutexLocker locker(&m_mutex);
                (void)sendSenderReportLocked(peer, static_cast<uint32_t>(sentFrame.pts));
            }
            if (!loggedFirstSentFrame && !sentFrame.payload.empty()) {
                qInfo().noquote() << "[audio-session] first RTP sent pts=" << sentFrame.pts << "samples=" << sentFrame.frameSamples;
                loggedFirstSentFrame = true;
            }
        }
        if (sendFailed) {
            continue;
        }
    }
}

void av::session::AudioCallSession::recvLoop() {
    bool loggedFirstReceivedPacket = false;
    std::uint8_t buffer[1500];
    while (m_running.load(std::memory_order_acquire)) {
        const int waitResult = m_mediaSocket.waitForReadable(-1);
        if (waitResult == 0) {
            continue;
        }
        if (waitResult < 0) {
            if (!m_running.load(std::memory_order_acquire) || m_mediaSocket.isTransientSocketError()) {
                continue;
            }
            setErrorLocked("wait readable failed");
            continue;
        }

        media::UdpEndpoint from{};
        const int received = m_mediaSocket.recvFrom(buffer, sizeof(buffer), from);
        if (received <= 0) {
            if (!m_running.load(std::memory_order_acquire) || m_mediaSocket.isTransientSocketError()) {
                continue;
            }
            setErrorLocked("recvfrom failed");
            continue;
        }

        const std::size_t receivedSize = static_cast<std::size_t>(received);
        const QByteArray receivedDatagram(reinterpret_cast<const char*>(buffer), received);
        if (media::DtlsTransportClient::looksLikeDtlsRecord(receivedDatagram)) {
            QMutexLocker locker(&m_mutex);
            (void)handleDtlsPacketLocked(buffer, receivedSize, from);
            continue;
        }
        if (looksLikeStunPacket(buffer, receivedSize)) {
            m_iceConnected.store(true, std::memory_order_release);
            qInfo().noquote() << "[audio-session] ice-connected binding-response received";
            continue;
        }

        std::vector<uint8_t> mediaPacket(buffer, buffer + received);
        if (media::looksLikeRtcpPacket(mediaPacket.data(), mediaPacket.size())) {
            QMutexLocker locker(&m_mutex);
            if (!unprotectRtcpLocked(&mediaPacket)) {
                continue;
            }
            (void)handleRtcpPacketLocked(mediaPacket.data(), mediaPacket.size(), from);
            continue;
        }

        {
            QMutexLocker locker(&m_mutex);
            if (!unprotectRtpLocked(&mediaPacket)) {
                continue;
            }
        }

        media::RTPPacket packet;
        if (!m_receiver.parsePacket(mediaPacket.data(), mediaPacket.size(), packet)) {
            setErrorLocked("RTP parse failed");
            continue;
        }

        m_receivedPacketCount.fetch_add(1, std::memory_order_relaxed);
        m_lastReceivedSequence.store(packet.header.sequenceNumber, std::memory_order_release);

        if (!m_jitter.push(packet)) {
            setErrorLocked("jitter push failed");
            continue;
        }
        if (!loggedFirstReceivedPacket) {
            qInfo().noquote() << "[audio-session] first RTP recv seq=" << packet.header.sequenceNumber << " ts=" << packet.header.timestamp << " bytes=" << packet.payload.size();
            loggedFirstReceivedPacket = true;
        }

        drainReceivedPackets();
    }
}

bool av::session::runAudioCallSessionLoopbackSelfCheck(std::string* error) {
    std::unique_ptr<QCoreApplication> ownedApplication;
    int appArgc = 1;
    char appName[] = "audio_call_self_check";
    char* appArgv[] = {appName, nullptr};
    if (QCoreApplication::instance() == nullptr) {
        ownedApplication = std::make_unique<QCoreApplication>(appArgc, appArgv);
    }

    AudioCallSessionConfig config{};
    config.peerAddress = "127.0.0.1";

    AudioCallSession session(config);
    if (!session.start()) {
        if (error != nullptr) {
            *error = session.lastError();
        }
        session.stop();
        return false;
    }

    const uint16_t localPort = session.localPort();
    if (localPort == 0) {
        if (error != nullptr) {
            *error = "local port not assigned";
        }
        session.stop();
        return false;
    }

    session.setPeer("127.0.0.1", localPort);

    const int frameSamples = session.player().frameSamples();

    av::capture::AudioFrame frame;
    frame.sampleRate = 48000;
    frame.channels = 1;
    frame.samples.assign(static_cast<std::size_t>(frameSamples), 0.2F);

    for (int i = 0; i < 20; ++i) {
        av::capture::AudioFrame submitFrame = frame;
        submitFrame.pts = static_cast<int64_t>(frameSamples * i);
        if (!session.submitLocalFrame(submitFrame)) {
            if (error != nullptr) {
                *error = "submit local frame failed";
            }
            session.stop();
            return false;
        }
    }

    av::capture::AudioFrame played;
    const bool gotPlayed = session.waitForPlayedFrame(played, std::chrono::milliseconds(5000));
    uint32_t observedRttMs = 0;
    for (int i = 0; i < 40 && observedRttMs == 0; ++i) {
        observedRttMs = session.lastRttMs();
        if (observedRttMs != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const uint32_t observedTargetBitrate = session.targetBitrateBps();
    const bool ok = gotPlayed &&
                    played.sampleRate == 48000 &&
                    played.channels == 1 &&
                    played.samples.size() == static_cast<std::size_t>(frameSamples) &&
                    observedRttMs > 0 &&
                    observedTargetBitrate >= 16000U &&
                    observedTargetBitrate <= 64000U;

    if (!ok && error != nullptr) {
        if (!session.lastError().empty()) {
            *error = session.lastError();
        } else if (!gotPlayed) {
            *error = "playback timeout";
        } else if (observedRttMs == 0) {
            *error = "rtcp rtt timeout";
        } else if (observedTargetBitrate < 16000U || observedTargetBitrate > 64000U) {
            *error = "audio bwe bitrate out of range";
        } else {
            *error = "unexpected decoded frame shape";
        }
    }

    session.stop();
    return ok;
}
