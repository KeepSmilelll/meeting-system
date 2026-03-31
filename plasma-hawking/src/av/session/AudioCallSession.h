#pragma once

#include "av/capture/AudioCapture.h"
#include "av/codec/AudioDecoder.h"
#include "av/codec/AudioEncoder.h"
#include "av/render/AudioPlayer.h"
#include "net/media/JitterBuffer.h"
#include "net/media/RTPReceiver.h"
#include "net/media/RTPSender.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <QDebug>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace av::session {

struct AudioCallSessionConfig {
    std::string localAddress{"0.0.0.0"};
    uint16_t localPort{0};
    std::string peerAddress{"127.0.0.1"};
    uint16_t peerPort{0};
    int sampleRate{48000};
    int channels{1};
    int frameSamples{960};
    int bitrate{32000};
    std::size_t jitterPackets{32};
};

class AudioCallSession {
public:
    explicit AudioCallSession(AudioCallSessionConfig config = {});
    ~AudioCallSession();

    bool start();
    void stop();

    bool submitLocalFrame(av::capture::AudioFrame frame);
    bool waitForPlayedFrame(av::capture::AudioFrame& outFrame, std::chrono::milliseconds timeout);

    void setCaptureMuted(bool muted);
    bool captureMuted() const;

    void setPeer(const std::string& address, uint16_t port);
    uint16_t localPort() const;
    bool isRunning() const;
    std::string lastError() const;

    av::render::AudioPlayer& player();
    const av::render::AudioPlayer& player() const;
    std::shared_ptr<av::sync::AVSync> clock() const;

private:
#ifdef _WIN32
    bool openSocketLocked();
    void closeSocketLocked();
    void sendLoop();
    void recvLoop();
    bool resolvePeerLocked(sockaddr_in& outPeer) const;
    bool encodeAndSend(av::codec::AudioEncoder& encoder, const av::capture::AudioFrame& frame, const sockaddr_in& peer, av::codec::EncodedAudioFrame* sentFrame = nullptr);
    void drainReceivedPackets();
    void queueDecodedFrameForPlayback(av::capture::AudioFrame frame);
    void setErrorLocked(std::string message);
#endif

    AudioCallSessionConfig m_config;
    av::capture::AudioCapture m_capture;
    av::codec::AudioEncoder m_encoder;
    av::codec::AudioDecoder m_decoder;
    media::RTPSender m_sender;
    media::RTPReceiver m_receiver;
    media::JitterBuffer m_jitter;
    av::render::AudioPlayer m_player;
    av::capture::AudioFrame m_playoutAssembler;

    mutable std::mutex m_mutex;
    std::thread m_sendThread;
    std::thread m_recvThread;
    std::string m_lastError;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_captureMuted{false};

#ifdef _WIN32
    SOCKET m_socket{INVALID_SOCKET};
    sockaddr_in m_peer{};
    bool m_peerValid{false};
#endif
};

bool runAudioCallSessionLoopbackSelfCheck(std::string* error = nullptr);

}  // namespace av::session

inline av::session::AudioCallSession::AudioCallSession(AudioCallSessionConfig config)
    : m_config(std::move(config)),
      m_capture(32),
      m_jitter(m_config.jitterPackets),
      m_player(32) {
    if (m_config.frameSamples <= 0) {
        m_config.frameSamples = 960;
    }
    if (m_config.channels != 1 && m_config.channels != 2) {
        m_config.channels = 1;
    }
}

inline av::session::AudioCallSession::~AudioCallSession() {
    stop();
}

inline bool av::session::AudioCallSession::start() {
#ifdef _WIN32
    if (m_running.load(std::memory_order_acquire)) {
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

    m_playoutAssembler = {};
    m_jitter.clear();
    m_sender.setSequence(0);
    m_sender.setSSRC(static_cast<uint32_t>(std::random_device{}()));

    if (!m_capture.start() || !m_player.start()) {
        m_capture.stop();
        m_player.stop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError.clear();
        m_peerValid = false;
        if (!openSocketLocked()) {
            m_capture.stop();
            m_player.stop();
            return false;
        }
    }

    m_running.store(true, std::memory_order_release);
    qInfo().noquote() << "[audio-session] encoderFrameSamples=" << encoderFrameSamples << "playoutFrameSamples=" << captureFrameSamples << "bitrate=" << m_config.bitrate;
    qInfo().noquote() << "[audio-session] started localPort=" << localPort();
    m_sendThread = std::thread(&AudioCallSession::sendLoop, this);
    m_recvThread = std::thread(&AudioCallSession::recvLoop, this);
    return true;
#else
    return false;
#endif
}

inline void av::session::AudioCallSession::stop() {
#ifdef _WIN32
    if (!m_running.exchange(false, std::memory_order_acq_rel) && !m_sendThread.joinable() && !m_recvThread.joinable()) {
        return;
    }

    m_capture.stop();
    m_player.stop();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        closeSocketLocked();
    }

    if (m_sendThread.joinable()) {
        m_sendThread.join();
    }
    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }
#endif
}

inline bool av::session::AudioCallSession::submitLocalFrame(av::capture::AudioFrame frame) {
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

inline bool av::session::AudioCallSession::waitForPlayedFrame(av::capture::AudioFrame& outFrame, std::chrono::milliseconds timeout) {
    return m_player.waitForPlayedFrame(outFrame, timeout);
}

inline void av::session::AudioCallSession::setCaptureMuted(bool muted) {
    m_captureMuted.store(muted, std::memory_order_release);
}

inline bool av::session::AudioCallSession::captureMuted() const {
    return m_captureMuted.load(std::memory_order_acquire);
}

inline void av::session::AudioCallSession::setPeer(const std::string& address, uint16_t port) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.peerAddress = address;
    m_config.peerPort = port;
    m_peerValid = false;
    if (m_socket != INVALID_SOCKET) {
        sockaddr_in peer{};
        if (resolvePeerLocked(peer)) {
            m_peer = peer;
            m_peerValid = true;
            qInfo().noquote() << "[audio-session] peer=" << QString::fromStdString(address) << ":" << port;
        }
    }
#else
    (void)address;
    (void)port;
#endif
}

inline uint16_t av::session::AudioCallSession::localPort() const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_socket == INVALID_SOCKET) {
        return 0;
    }
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
#else
    return 0;
#endif
}

inline bool av::session::AudioCallSession::isRunning() const {
    return m_running.load(std::memory_order_acquire);
}

inline std::string av::session::AudioCallSession::lastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

inline av::render::AudioPlayer& av::session::AudioCallSession::player() {
    return m_player;
}

inline const av::render::AudioPlayer& av::session::AudioCallSession::player() const {
    return m_player;
}

inline std::shared_ptr<av::sync::AVSync> av::session::AudioCallSession::clock() const {
    return m_player.clock();
}

#ifdef _WIN32
inline bool av::session::AudioCallSession::openSocketLocked() {
    static std::once_flag wsaInitOnce;
    static int wsaInitStatus = 0;
    std::call_once(wsaInitOnce, [] {
        WSADATA data{};
        wsaInitStatus = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (wsaInitStatus != 0) {
        setErrorLocked("WSAStartup failed");
        return false;
    }

    m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        setErrorLocked("socket() failed");
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(m_config.localPort);
    if (m_config.localAddress.empty() || m_config.localAddress == "0.0.0.0") {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (InetPtonA(AF_INET, m_config.localAddress.c_str(), &local.sin_addr) != 1) {
        setErrorLocked("invalid local address");
        closeSocketLocked();
        return false;
    }

    if (::bind(m_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        setErrorLocked("bind() failed");
        closeSocketLocked();
        return false;
    }

    sockaddr_in peer{};
    if (resolvePeerLocked(peer)) {
        m_peer = peer;
        m_peerValid = true;
    }

    DWORD timeoutMs = 50;
    ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    return true;
}

inline void av::session::AudioCallSession::closeSocketLocked() {
    if (m_socket != INVALID_SOCKET) {
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_peerValid = false;
}

inline bool av::session::AudioCallSession::resolvePeerLocked(sockaddr_in& outPeer) const {
    if (m_config.peerAddress.empty() || m_config.peerPort == 0) {
        return false;
    }

    std::memset(&outPeer, 0, sizeof(outPeer));
    outPeer.sin_family = AF_INET;
    outPeer.sin_port = htons(m_config.peerPort);
    if (InetPtonA(AF_INET, m_config.peerAddress.c_str(), &outPeer.sin_addr) != 1) {
        return false;
    }
    return true;
}

inline bool av::session::AudioCallSession::encodeAndSend(av::codec::AudioEncoder& encoder, const av::capture::AudioFrame& frame, const sockaddr_in& peer, av::codec::EncodedAudioFrame* sentFrame) {
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

    const auto packet = m_sender.buildPacket(encoded.payloadType,
                                             true,
                                             static_cast<uint32_t>(encoded.pts),
                                             encoded.payload);
    if (packet.empty()) {
        setErrorLocked("RTP build failed");
        return false;
    }

    const int sent = ::sendto(m_socket,
                              reinterpret_cast<const char*>(packet.data()),
                              static_cast<int>(packet.size()),
                              0,
                              reinterpret_cast<const sockaddr*>(&peer),
                              sizeof(peer));
    if (sent != static_cast<int>(packet.size())) {
        setErrorLocked("sendto failed");
        return false;
    }
    if (sentFrame != nullptr) {
        *sentFrame = std::move(encoded);
    }
    return true;
}

inline void av::session::AudioCallSession::drainReceivedPackets() {
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

inline void av::session::AudioCallSession::queueDecodedFrameForPlayback(av::capture::AudioFrame frame) {
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
inline void av::session::AudioCallSession::setErrorLocked(std::string message) {
    m_lastError = std::move(message);
    qWarning().noquote() << "[audio-session]" << QString::fromStdString(m_lastError);
}

inline void av::session::AudioCallSession::sendLoop() {
    bool loggedFirstSentFrame = false;
    av::codec::AudioEncoder encoder;
    if (!encoder.configure(m_config.sampleRate, m_config.channels, m_config.bitrate)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setErrorLocked("send encoder configure failed");
        return;
    }

    while (m_running.load(std::memory_order_acquire)) {
        av::capture::AudioFrame frame;
        if (!m_capture.popFrameForEncode(frame, std::chrono::milliseconds(50))) {
            continue;
        }
        if (m_captureMuted.load(std::memory_order_acquire)) {
            continue;
        }

        sockaddr_in peer{};
        bool peerReady = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            peerReady = m_peerValid;
            if (peerReady) {
                peer = m_peer;
            }
        }
        if (!peerReady) {
            continue;
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

inline void av::session::AudioCallSession::recvLoop() {
    bool loggedFirstReceivedPacket = false;
    std::uint8_t buffer[1500];
    while (m_running.load(std::memory_order_acquire)) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int received = ::recvfrom(m_socket,
                                        reinterpret_cast<char*>(buffer),
                                        static_cast<int>(sizeof(buffer)),
                                        0,
                                        reinterpret_cast<sockaddr*>(&from),
                                        &fromLen);
        if (received <= 0) {
            const int err = WSAGetLastError();
            if (!m_running.load(std::memory_order_acquire) || err == WSAETIMEDOUT || err == WSAEINTR) {
                continue;
            }
            setErrorLocked("recvfrom failed");
            continue;
        }

        media::RTPPacket packet;
        if (!m_receiver.parsePacket(buffer, static_cast<std::size_t>(received), packet)) {
            setErrorLocked("RTP parse failed");
            continue;
        }

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
#endif

inline bool av::session::runAudioCallSessionLoopbackSelfCheck(std::string* error) {
#ifdef _WIN32
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
    const bool ok = gotPlayed &&
                    played.sampleRate == 48000 &&
                    played.channels == 1 &&
                    played.samples.size() == static_cast<std::size_t>(frameSamples);

    if (!ok && error != nullptr) {
        if (!session.lastError().empty()) {
            *error = session.lastError();
        } else if (!gotPlayed) {
            *error = "playback timeout";
        } else {
            *error = "unexpected decoded frame shape";
        }
    }

    session.stop();
    return ok;
#else
    return false;
#endif
}





















