#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <chrono>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QUdpSocket>

#include "av/session/ScreenShareSession.h"

namespace {

bool waitForCondition(QCoreApplication& app, const std::function<bool()>& condition, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (condition()) {
            return true;
        }
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return condition();
}

std::vector<uint8_t> buildPliPacket(uint32_t senderSsrc, uint32_t mediaSsrc) {
    return std::vector<uint8_t>{
        0x81, 0xCE, 0x00, 0x02,
        static_cast<uint8_t>((senderSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((senderSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((senderSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(senderSsrc & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(mediaSsrc & 0xFF),
    };
}

std::vector<uint8_t> buildRembPacket(uint32_t senderSsrc, uint32_t targetSsrc, uint32_t bitrateBps) {
    uint8_t exp = 0;
    uint32_t mantissa = bitrateBps;
    while (mantissa > 0x3FFFFU && exp < 63U) {
        mantissa = (mantissa + 1U) >> 1U;
        ++exp;
    }

    return std::vector<uint8_t>{
        0x8F, 0xCE, 0x00, 0x05,
        static_cast<uint8_t>((senderSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((senderSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((senderSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(senderSsrc & 0xFF),
        0x00, 0x00, 0x00, 0x00,
        'R', 'E', 'M', 'B',
        0x01,
        static_cast<uint8_t>((exp << 2U) | static_cast<uint8_t>((mantissa >> 16U) & 0x03U)),
        static_cast<uint8_t>((mantissa >> 8U) & 0xFFU),
        static_cast<uint8_t>(mantissa & 0xFFU),
        static_cast<uint8_t>((targetSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((targetSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((targetSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(targetSsrc & 0xFF),
    };
}

std::vector<uint8_t> buildNackPacket(uint32_t senderSsrc, uint32_t mediaSsrc, uint16_t pid, uint16_t blp) {
    return std::vector<uint8_t>{
        0x81, 0xCD, 0x00, 0x03,
        static_cast<uint8_t>((senderSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((senderSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((senderSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(senderSsrc & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(mediaSsrc & 0xFF),
        static_cast<uint8_t>((pid >> 8) & 0xFF),
        static_cast<uint8_t>(pid & 0xFF),
        static_cast<uint8_t>((blp >> 8) & 0xFF),
        static_cast<uint8_t>(blp & 0xFF),
    };
}

uint32_t compactNtpNow() {
    constexpr uint64_t kNtpUnixEpochOffsetSeconds = 2208988800ULL;
    const auto now = std::chrono::system_clock::now();
    const auto sinceEpoch = now.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - seconds);
    const uint64_t ntpSeconds =
        static_cast<uint64_t>(seconds.count()) + kNtpUnixEpochOffsetSeconds;
    const uint64_t ntpFraction =
        (static_cast<uint64_t>(nanoseconds.count()) << 32U) / 1000000000ULL;
    return static_cast<uint32_t>((((ntpSeconds << 32U) | ntpFraction) >> 16U) & 0xFFFFFFFFU);
}

uint8_t fractionForLossPercent(double percent) {
    return static_cast<uint8_t>((percent * 256.0) / 100.0 + 0.5);
}

std::vector<uint8_t> buildReceiverReportPacket(uint32_t receiverSsrc,
                                               uint32_t mediaSsrc,
                                               uint8_t fractionLost,
                                               uint32_t lastSenderReport = 0U,
                                               uint32_t delaySinceLastSenderReport = 0U) {
    return std::vector<uint8_t>{
        0x81, 0xC9, 0x00, 0x07,
        static_cast<uint8_t>((receiverSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((receiverSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((receiverSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(receiverSsrc & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(mediaSsrc & 0xFF),
        fractionLost,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        static_cast<uint8_t>((lastSenderReport >> 24) & 0xFF),
        static_cast<uint8_t>((lastSenderReport >> 16) & 0xFF),
        static_cast<uint8_t>((lastSenderReport >> 8) & 0xFF),
        static_cast<uint8_t>(lastSenderReport & 0xFF),
        static_cast<uint8_t>((delaySinceLastSenderReport >> 24) & 0xFF),
        static_cast<uint8_t>((delaySinceLastSenderReport >> 16) & 0xFF),
        static_cast<uint8_t>((delaySinceLastSenderReport >> 8) & 0xFF),
        static_cast<uint8_t>(delaySinceLastSenderReport & 0xFF),
    };
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("MEETING_SYNTHETIC_SCREEN")) {
        qputenv("MEETING_SYNTHETIC_SCREEN", QByteArrayLiteral("1"));
    }
    if (qEnvironmentVariableIsEmpty("MEETING_SYNTHETIC_CAMERA")) {
        qputenv("MEETING_SYNTHETIC_CAMERA", QByteArrayLiteral("1"));
    }

    av::session::ScreenShareSessionConfig senderConfig{};
    senderConfig.localAddress = "127.0.0.1";
    senderConfig.peerAddress = "127.0.0.1";
    senderConfig.width = 640;
    senderConfig.height = 360;
    senderConfig.frameRate = 5;
    senderConfig.bitrate = 800 * 1000;

    av::session::ScreenShareSessionConfig receiverConfig = senderConfig;

    av::session::ScreenShareSession receiver(receiverConfig);
    bool decodedFrameReady = false;
    int decodedWidth = 0;
    int decodedHeight = 0;
    receiver.setDecodedFrameCallback([&](av::codec::DecodedVideoFrame frame) {
        decodedWidth = frame.width;
        decodedHeight = frame.height;
        decodedFrameReady = true;
    });
    assert(receiver.start());
    assert(receiver.localPort() != 0);

    av::session::ScreenShareSession sender(senderConfig);
    int adaptiveTurnRequests = 0;
    sender.setAdaptiveTurnRelayRequestCallback([&adaptiveTurnRequests]() {
        ++adaptiveTurnRequests;
    });
    assert(sender.start());
    assert(sender.localPort() != 0);

    sender.setPeer("127.0.0.1", receiver.localPort());
    receiver.setPeer("127.0.0.1", sender.localPort());
    assert(sender.setSharingEnabled(true));
    assert(sender.videoSsrc() != 0);
    const uint32_t screenSsrc = sender.videoSsrc();

    const bool packetsSent = waitForCondition(app, [&sender]() {
        return sender.sentPacketCount() > 0;
    }, 5000);
    const std::string senderError = sender.lastError();
    const bool encoderUnavailable = !packetsSent &&
                                    senderError.find("video encoder configure failed") != std::string::npos;
    if (!packetsSent) {
        qCritical().noquote() << "screen sender produced no RTP packets"
                              << "sent=" << sender.sentPacketCount()
                              << "recv=" << receiver.receivedPacketCount()
                              << "senderError=" << QString::fromStdString(sender.lastError())
                              << "receiverError=" << QString::fromStdString(receiver.lastError());
        std::cerr << "screen sender produced no RTP packets"
                  << " sent=" << sender.sentPacketCount()
                  << " recv=" << receiver.receivedPacketCount()
                  << " senderError=" << sender.lastError()
                  << " receiverError=" << receiver.lastError()
                  << std::endl;
    }
    assert(packetsSent || encoderUnavailable);

    if (packetsSent) {
        const bool packetsReceived = waitForCondition(app, [&receiver]() {
            return receiver.receivedPacketCount() > 0;
        }, 5000);
        assert(packetsReceived);

        const bool frameDecoded = waitForCondition(app, [&decodedFrameReady]() {
            return decodedFrameReady;
        }, 5000);
        assert(frameDecoded);
        assert(decodedWidth == 640);
        assert(decodedHeight == 360);

        receiver.setExpectedRemoteVideoSsrc(screenSsrc + 1U);
        // Recv now runs per-SSRC worker queues; allow a tiny in-flight tail after filter switch.
        QThread::msleep(120);
        app.processEvents(QEventLoop::AllEvents, 20);
        const uint64_t filteredReceiveBase = receiver.receivedPacketCount();
        const uint64_t filteredSendBase = sender.sentPacketCount();

        const bool senderStillSending = waitForCondition(app, [&sender, filteredSendBase]() {
            return sender.sentPacketCount() > filteredSendBase + 5;
        }, 3000);
        assert(senderStillSending);

        constexpr uint64_t kAllowedInFlightTailPackets = 1U;
        const bool receivedUnexpectedSsrc = waitForCondition(app, [&receiver, filteredReceiveBase]() {
            return receiver.receivedPacketCount() > (filteredReceiveBase + kAllowedInFlightTailPackets);
        }, 800);
        assert(!receivedUnexpectedSsrc);

        receiver.setExpectedRemoteVideoSsrc(screenSsrc);
        const bool receiveRecovered = waitForCondition(app, [&receiver, filteredReceiveBase]() {
            return receiver.receivedPacketCount() > filteredReceiveBase;
        }, 5000);
        assert(receiveRecovered);
    }

    assert(sender.setSharingEnabled(false));

    const uint64_t cameraSentBase = sender.sentPacketCount();
    const uint64_t cameraRecvBase = receiver.receivedPacketCount();
    decodedFrameReady = false;
    const bool cameraEnabled = sender.setCameraSendingEnabled(true);
    assert(cameraEnabled);
    assert(sender.cameraSendingEnabled());
    assert(sender.videoSsrc() != 0);
    const uint32_t cameraSsrc = sender.videoSsrc();
    receiver.setExpectedRemoteVideoSsrc(cameraSsrc);

    const bool cameraPacketsSent = waitForCondition(app, [&sender, cameraSentBase]() {
        return sender.sentPacketCount() > cameraSentBase;
    }, 5000);
    const std::string cameraError = sender.lastError();
    const bool cameraEncoderUnavailable = !cameraPacketsSent &&
                                          cameraError.find("video encoder configure failed") != std::string::npos;
    assert(cameraPacketsSent || cameraEncoderUnavailable);
    if (cameraPacketsSent) {
        const bool cameraPacketsReceived = waitForCondition(app, [&receiver, cameraRecvBase]() {
            return receiver.receivedPacketCount() > cameraRecvBase;
        }, 5000);
        assert(cameraPacketsReceived);
        const bool cameraFrameDecoded = waitForCondition(app, [&decodedFrameReady]() {
            return decodedFrameReady;
        }, 5000);
        assert(cameraFrameDecoded);
    }

    assert(sender.setCameraSendingEnabled(false));
    assert(!sender.cameraSendingEnabled());

    uint32_t rtcpFeedbackSsrc = sender.videoSsrc();
    if (rtcpFeedbackSsrc == 0U) {
        rtcpFeedbackSsrc = cameraSsrc != 0U ? cameraSsrc : screenSsrc;
    }

    if (!packetsSent) {
        sender.stop();
        receiver.stop();
        return 0;
    }

    QUdpSocket feedbackSocket;
    assert(feedbackSocket.bind(QHostAddress::LocalHost, 0));


    const auto pliPacket = buildPliPacket(0x33333333U, rtcpFeedbackSsrc);
    const qint64 pliSent = feedbackSocket.writeDatagram(reinterpret_cast<const char*>(pliPacket.data()),
                                                        static_cast<qint64>(pliPacket.size()),
                                                        QHostAddress::LocalHost,
                                                        sender.localPort());
    assert(pliSent == static_cast<qint64>(pliPacket.size()));

    const bool pliHandled = waitForCondition(app, [&sender]() {
        return sender.keyframeRequestCount() > 0;
    }, 2000);
    assert(pliHandled);

    if (packetsSent) {
        const uint64_t retransmitBase = sender.retransmitPacketCount();
        const auto nackPacket = buildNackPacket(0x33333333U, rtcpFeedbackSsrc, 0U, 0U);
        const qint64 nackSent = feedbackSocket.writeDatagram(reinterpret_cast<const char*>(nackPacket.data()),
                                                             static_cast<qint64>(nackPacket.size()),
                                                             QHostAddress::LocalHost,
                                                             sender.localPort());
        assert(nackSent == static_cast<qint64>(nackPacket.size()));

        const bool nackHandled = waitForCondition(app, [&sender, retransmitBase]() {
            return sender.retransmitPacketCount() > retransmitBase;
        }, 2000);
        assert(nackHandled);
    }

    constexpr uint32_t kTargetBitrateBps = 200000U;
    const auto rembPacket = buildRembPacket(0x33333333U, rtcpFeedbackSsrc, kTargetBitrateBps);
    const qint64 rembSent = feedbackSocket.writeDatagram(reinterpret_cast<const char*>(rembPacket.data()),
                                                         static_cast<qint64>(rembPacket.size()),
                                                         QHostAddress::LocalHost,
                                                         sender.localPort());
    assert(rembSent == static_cast<qint64>(rembPacket.size()));

    const bool rembTargetHandled = waitForCondition(app, [&sender, kTargetBitrateBps]() {
        return sender.targetBitrateBps() == kTargetBitrateBps;
    }, 2000);
    assert(rembTargetHandled);

    const bool senderActiveForBitrateApply = sender.sharingEnabled() || sender.cameraSendingEnabled();
    if (packetsSent && senderActiveForBitrateApply) {
        const uint64_t reconfigureBase = sender.bitrateReconfigureCount();
        const bool rembApplied = waitForCondition(app, [&sender, kTargetBitrateBps]() {
            return sender.appliedBitrateBps() == kTargetBitrateBps;
        }, 4000);
        assert(rembApplied);
        assert(sender.bitrateReconfigureCount() > reconfigureBase);
        const uint64_t firstReconfigureCount = sender.bitrateReconfigureCount();
        assert(sender.lastBitrateApplyDelayMs() <= 5000U);

        constexpr uint32_t kFollowupBitrateBps = 350000U;
        const auto followupRembPacket = buildRembPacket(0x44444444U, rtcpFeedbackSsrc, kFollowupBitrateBps);
        const qint64 followupRembSent = feedbackSocket.writeDatagram(reinterpret_cast<const char*>(followupRembPacket.data()),
                                                                     static_cast<qint64>(followupRembPacket.size()),
                                                                     QHostAddress::LocalHost,
                                                                     sender.localPort());
        assert(followupRembSent == static_cast<qint64>(followupRembPacket.size()));

        const bool followupTargetHandled = waitForCondition(app, [&sender, kFollowupBitrateBps]() {
            return sender.targetBitrateBps() == kFollowupBitrateBps;
        }, 2000);
        assert(followupTargetHandled);

        const bool followupApplied = waitForCondition(app, [&sender, kFollowupBitrateBps]() {
            return sender.appliedBitrateBps() == kFollowupBitrateBps;
        }, 4000);
        assert(followupApplied);
        assert(sender.bitrateReconfigureCount() > firstReconfigureCount);
        assert(sender.lastBitrateApplyDelayMs() <= 5000U);
    }

    const auto moderateRrPacket = buildReceiverReportPacket(0x55555555U,
                                                            rtcpFeedbackSsrc,
                                                            fractionForLossPercent(6.0));
    const qint64 moderateRrSent = feedbackSocket.writeDatagram(
        reinterpret_cast<const char*>(moderateRrPacket.data()),
        static_cast<qint64>(moderateRrPacket.size()),
        QHostAddress::LocalHost,
        sender.localPort());
    assert(moderateRrSent == static_cast<qint64>(moderateRrPacket.size()));
    const bool moderateRrHandled = waitForCondition(app, [&sender]() {
        return sender.adaptiveVideoFrameRate() == 15 && !sender.adaptiveVideoSuspended();
    }, 2000);
    assert(moderateRrHandled);

    const auto severeRrPacket = buildReceiverReportPacket(0x55555555U,
                                                          rtcpFeedbackSsrc,
                                                          fractionForLossPercent(16.0));
    const qint64 severeRrSent = feedbackSocket.writeDatagram(
        reinterpret_cast<const char*>(severeRrPacket.data()),
        static_cast<qint64>(severeRrPacket.size()),
        QHostAddress::LocalHost,
        sender.localPort());
    assert(severeRrSent == static_cast<qint64>(severeRrPacket.size()));
    const bool severeRrHandled = waitForCondition(app, [&sender]() {
        return sender.adaptiveVideoHeight() == 360 && sender.targetBitrateBps() <= 300000U;
    }, 2000);
    assert(severeRrHandled);

    const auto audioOnlyRrPacket = buildReceiverReportPacket(0x55555555U,
                                                            rtcpFeedbackSsrc,
                                                            fractionForLossPercent(31.0));
    const qint64 audioOnlyRrSent = feedbackSocket.writeDatagram(
        reinterpret_cast<const char*>(audioOnlyRrPacket.data()),
        static_cast<qint64>(audioOnlyRrPacket.size()),
        QHostAddress::LocalHost,
        sender.localPort());
    assert(audioOnlyRrSent == static_cast<qint64>(audioOnlyRrPacket.size()));
    const bool audioOnlyRrHandled = waitForCondition(app, [&sender]() {
        return sender.adaptiveVideoSuspended() && sender.targetBitrateBps() == 0U;
    }, 2000);
    assert(audioOnlyRrHandled);

    const uint32_t rttUnits700Ms = static_cast<uint32_t>((700ULL * 65536ULL) / 1000ULL);
    const uint32_t syntheticLsr = compactNtpNow() - rttUnits700Ms;
    const auto highRttRrPacket = buildReceiverReportPacket(0x55555555U,
                                                           rtcpFeedbackSsrc,
                                                           fractionForLossPercent(1.0),
                                                           syntheticLsr,
                                                           0U);
    const qint64 highRttRrSent = feedbackSocket.writeDatagram(
        reinterpret_cast<const char*>(highRttRrPacket.data()),
        static_cast<qint64>(highRttRrPacket.size()),
        QHostAddress::LocalHost,
        sender.localPort());
    assert(highRttRrSent == static_cast<qint64>(highRttRrPacket.size()));
    const bool highRttHandled = waitForCondition(app, [&sender, &adaptiveTurnRequests]() {
        return sender.adaptiveJitterTargetMs() == 100U &&
               sender.adaptiveTurnRelayRequested() &&
               adaptiveTurnRequests == 1;
    }, 2000);
    assert(highRttHandled);

    sender.stop();
    receiver.stop();
    return 0;
}




