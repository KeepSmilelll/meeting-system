#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
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

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qputenv("MEETING_SYNTHETIC_SCREEN", QByteArrayLiteral("1"));

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
    assert(sender.start());
    assert(sender.localPort() != 0);

    sender.setPeer("127.0.0.1", receiver.localPort());
    assert(sender.setSharingEnabled(true));
    assert(sender.videoSsrc() != 0);

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
    }

    QUdpSocket feedbackSocket;
    assert(feedbackSocket.bind(QHostAddress::LocalHost, 0));

    const uint32_t screenSsrc = sender.videoSsrc();
    assert(screenSsrc != 0);

    const auto pliPacket = buildPliPacket(0x33333333U, screenSsrc);
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
        const auto nackPacket = buildNackPacket(0x33333333U, screenSsrc, 0U, 0U);
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
    const auto rembPacket = buildRembPacket(0x33333333U, screenSsrc, kTargetBitrateBps);
    const qint64 rembSent = feedbackSocket.writeDatagram(reinterpret_cast<const char*>(rembPacket.data()),
                                                         static_cast<qint64>(rembPacket.size()),
                                                         QHostAddress::LocalHost,
                                                         sender.localPort());
    assert(rembSent == static_cast<qint64>(rembPacket.size()));

    const bool rembTargetHandled = waitForCondition(app, [&sender, kTargetBitrateBps]() {
        return sender.targetBitrateBps() == kTargetBitrateBps;
    }, 2000);
    assert(rembTargetHandled);

    if (packetsSent) {
        const uint64_t reconfigureBase = sender.bitrateReconfigureCount();
        const bool rembApplied = waitForCondition(app, [&sender, kTargetBitrateBps]() {
            return sender.appliedBitrateBps() == kTargetBitrateBps;
        }, 4000);
        assert(rembApplied);
        assert(sender.bitrateReconfigureCount() > reconfigureBase);
        const uint64_t firstReconfigureCount = sender.bitrateReconfigureCount();
        assert(sender.lastBitrateApplyDelayMs() <= 5000U);

        constexpr uint32_t kFollowupBitrateBps = 350000U;
        const auto followupRembPacket = buildRembPacket(0x44444444U, screenSsrc, kFollowupBitrateBps);
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

    sender.stop();
    receiver.stop();
    return 0;
}
