#pragma once

#include "av/capture/AudioCapture.h"
#include "av/codec/AudioDecoder.h"
#include "av/codec/AudioEncoder.h"
#include "av/process/AcousticEchoCanceller.h"
#include "av/process/AutoGainControl.h"
#include "av/process/NoiseSuppressor.h"
#include "av/render/AudioPlayer.h"
#include "net/media/BandwidthEstimator.h"
#include "net/media/DtlsTransportClient.h"
#include "net/media/JitterBuffer.h"
#include "net/media/RTCPHandler.h"
#include "net/media/RTPReceiver.h"
#include "net/media/RTPSender.h"
#include "net/media/UdpPeerSocket.h"
#include "security/SRTPContext.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <QByteArray>
#include <QString>
#include <QMutex>
#include <QWaitCondition>

class QThread;

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
    bool setPreferredInputDeviceName(const QString& deviceName);
    bool setPreferredOutputDeviceName(const QString& deviceName);
    QString preferredInputDeviceName() const;
    QString preferredOutputDeviceName() const;

    void setPeer(const std::string& address, uint16_t port);
    void setAudioSsrc(uint32_t ssrc);
    QString prepareDtlsFingerprint();
    bool startDtlsSrtp(const QString& serverFingerprint);
    bool iceConnected() const;
    bool dtlsConnected() const;
    bool srtpReady() const;
    bool sendTransportProbe(const std::vector<uint8_t>& packet);
    uint16_t localPort() const;
    uint32_t audioSsrc() const;
    uint32_t lastRttMs() const;
    uint32_t targetBitrateBps() const;
    uint64_t sentPacketCount() const;
    uint64_t receivedPacketCount() const;
    uint64_t playedFrameCount() const;
    bool isRunning() const;
    std::string lastError() const;

    av::render::AudioPlayer& player();
    const av::render::AudioPlayer& player() const;
    std::shared_ptr<av::sync::AVSync> clock() const;

private:
    struct SenderReportLedger {
        uint32_t ssrc{0};
        uint32_t compactNtp{0};
        std::chrono::steady_clock::time_point observedAt{};
    };

    bool openSocketLocked();
    void closeSocketLocked();
    void sendLoop();
    void recvLoop();
    bool encodeAndSend(av::codec::AudioEncoder& encoder, const av::capture::AudioFrame& frame, const media::UdpEndpoint& peer, av::codec::EncodedAudioFrame* sentFrame = nullptr);
    void drainReceivedPackets();
    void queueDecodedFrameForPlayback(av::capture::AudioFrame frame);
    bool sendSenderReportLocked(const media::UdpEndpoint& peer, uint32_t rtpTimestamp);
    bool sendReceiverReportLocked(const media::UdpEndpoint& peer, uint32_t remoteSsrc, uint32_t lastSenderReport, uint32_t delaySinceLastSenderReport);
    bool handleRtcpPacketLocked(const uint8_t* data, std::size_t len, const media::UdpEndpoint& from);
    bool handleDtlsPacketLocked(const uint8_t* data, std::size_t len, const media::UdpEndpoint& from);
    bool sendCachedDtlsHandshakeLocked(const media::UdpEndpoint& peer);
    bool configureSrtpLocked();
    bool protectRtpLocked(std::vector<uint8_t>* packet);
    bool protectRtcpLocked(std::vector<uint8_t>* packet);
    bool unprotectRtpLocked(std::vector<uint8_t>* packet);
    bool unprotectRtcpLocked(std::vector<uint8_t>* packet);
    static uint64_t currentNtpTimestamp();
    static uint32_t compactNtpFromTimestamp(uint64_t ntpTimestamp);
    static uint32_t compactNtpFromElapsed(std::chrono::steady_clock::duration elapsed);
    static uint32_t compactNtpToMs(uint32_t compactNtp);
    void setErrorLocked(std::string message);

    AudioCallSessionConfig m_config;
    av::capture::AudioCapture m_capture;
    av::codec::AudioEncoder m_encoder;
    av::codec::AudioDecoder m_decoder;
    media::RTPSender m_sender;
    media::RTPReceiver m_receiver;
    media::BandwidthEstimator m_audioBwe{media::BandwidthEstimator::Config{16U, 64U, 32U, 500U}};
    media::RTCPHandler m_rtcpHandler;
    media::JitterBuffer m_jitter;
    av::render::AudioPlayer m_player;
    av::capture::AudioFrame m_playoutAssembler;
    av::process::AcousticEchoCanceller m_aec;
    av::process::NoiseSuppressor m_noiseSuppressor;
    av::process::AutoGainControl m_autoGainControl;

    mutable QMutex m_mutex;
    QWaitCondition m_stateWaitCondition;
    QThread* m_sendThread{nullptr};
    QThread* m_recvThread{nullptr};
    std::string m_lastError;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_captureMuted{false};
    std::atomic<uint32_t> m_lastRttMs{0};
    std::atomic<uint32_t> m_targetBitrateBps{32000};
    std::atomic<uint32_t> m_lastReceivedSequence{0};
    std::atomic<uint32_t> m_receivedPacketCount{0};
    std::atomic<uint32_t> m_sentPacketCount{0};
    std::atomic<uint32_t> m_sentOctetCount{0};
    std::atomic<bool> m_dtlsStarted{false};
    std::atomic<bool> m_iceConnected{false};
    std::atomic<bool> m_dtlsConnected{false};
    std::atomic<bool> m_srtpReady{false};

    media::UdpPeerSocket m_mediaSocket;
    media::DtlsTransportClient m_dtlsTransport;
    std::vector<QByteArray> m_dtlsHandshakePackets;
    std::chrono::steady_clock::time_point m_lastDtlsHandshakeSendAt{};
    security::SRTPContext m_inboundSrtp;
    security::SRTPContext m_outboundSrtp;
    SenderReportLedger m_localSenderReport{};
    SenderReportLedger m_remoteSenderReport{};
};

bool runAudioCallSessionLoopbackSelfCheck(std::string* error = nullptr);

}  // namespace av::session

