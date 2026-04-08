#include <cassert>
#include <functional>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include "net/signaling/SignalProtocol.h"
#include "net/signaling/SignalingClient.h"
#include "signaling.pb.h"

namespace {

constexpr quint16 kMediaOffer = 0x0301;
constexpr quint16 kMediaAnswer = 0x0302;
constexpr quint16 kMediaMuteToggle = 0x0304;
constexpr quint16 kMediaScreenShare = 0x0305;

bool waitForCondition(QCoreApplication& app, const std::function<bool()>& condition, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (condition()) {
            return true;
        }
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return condition();
}

QByteArray readFrame(QCoreApplication& app, QTcpSocket& socket) {
    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000) {
        app.processEvents(QEventLoop::AllEvents, 20);
        buffer += socket.readAll();
        if (buffer.size() >= static_cast<int>(signaling::kHeaderSize)) {
            std::vector<uint8_t> header;
            header.reserve(signaling::kHeaderSize);
            for (int i = 0; i < static_cast<int>(signaling::kHeaderSize); ++i) {
                header.push_back(static_cast<uint8_t>(buffer.at(i)));
            }
            const auto decoded = signaling::decodeHeader(header, 1024 * 1024);
            if (decoded.has_value() && buffer.size() >= static_cast<int>(signaling::kHeaderSize + decoded->length)) {
                return buffer.left(static_cast<int>(signaling::kHeaderSize + decoded->length));
            }
        }
        QThread::msleep(10);
    }
    return buffer;
}

void assertOfferFrame(const QByteArray& frame, const QString& expectedTarget, const QString& expectedSdp, quint32 expectedAudioSsrc, quint32 expectedVideoSsrc) {
    assert(frame.size() >= static_cast<int>(signaling::kHeaderSize));

    std::vector<uint8_t> header;
    header.reserve(signaling::kHeaderSize);
    for (int i = 0; i < static_cast<int>(signaling::kHeaderSize); ++i) {
        header.push_back(static_cast<uint8_t>(frame.at(i)));
    }
    const auto decoded = signaling::decodeHeader(header, 1024 * 1024);
    assert(decoded.has_value());
    assert(decoded->type == kMediaOffer);

    meeting::MediaOffer offer;
    const QByteArray payload = frame.mid(static_cast<int>(signaling::kHeaderSize), static_cast<int>(decoded->length));
    assert(offer.ParseFromArray(payload.constData(), payload.size()));
    assert(QString::fromStdString(offer.target_user_id()) == expectedTarget);
    assert(QString::fromStdString(offer.sdp()) == expectedSdp);
    assert(offer.audio_ssrc() == expectedAudioSsrc);
    assert(offer.video_ssrc() == expectedVideoSsrc);
}

void assertAnswerFrame(const QByteArray& frame, const QString& expectedTarget, const QString& expectedSdp, quint32 expectedAudioSsrc, quint32 expectedVideoSsrc) {
    assert(frame.size() >= static_cast<int>(signaling::kHeaderSize));

    std::vector<uint8_t> header;
    header.reserve(signaling::kHeaderSize);
    for (int i = 0; i < static_cast<int>(signaling::kHeaderSize); ++i) {
        header.push_back(static_cast<uint8_t>(frame.at(i)));
    }
    const auto decoded = signaling::decodeHeader(header, 1024 * 1024);
    assert(decoded.has_value());
    assert(decoded->type == kMediaAnswer);

    meeting::MediaAnswer answer;
    const QByteArray payload = frame.mid(static_cast<int>(signaling::kHeaderSize), static_cast<int>(decoded->length));
    assert(answer.ParseFromArray(payload.constData(), payload.size()));
    assert(QString::fromStdString(answer.target_user_id()) == expectedTarget);
    assert(QString::fromStdString(answer.sdp()) == expectedSdp);
    assert(answer.audio_ssrc() == expectedAudioSsrc);
    assert(answer.video_ssrc() == expectedVideoSsrc);
}

void assertMuteToggleFrame(const QByteArray& frame, int expectedMediaType, bool expectedMuted) {
    assert(frame.size() >= static_cast<int>(signaling::kHeaderSize));

    std::vector<uint8_t> header;
    header.reserve(signaling::kHeaderSize);
    for (int i = 0; i < static_cast<int>(signaling::kHeaderSize); ++i) {
        header.push_back(static_cast<uint8_t>(frame.at(i)));
    }
    const auto decoded = signaling::decodeHeader(header, 1024 * 1024);
    assert(decoded.has_value());
    assert(decoded->type == kMediaMuteToggle);

    meeting::MediaMuteToggle toggle;
    const QByteArray payload = frame.mid(static_cast<int>(signaling::kHeaderSize), static_cast<int>(decoded->length));
    assert(toggle.ParseFromArray(payload.constData(), payload.size()));
    assert(toggle.media_type() == expectedMediaType);
    assert(toggle.muted() == expectedMuted);
}

void assertScreenShareFrame(const QByteArray& frame, bool expectedSharing) {
    assert(frame.size() >= static_cast<int>(signaling::kHeaderSize));

    std::vector<uint8_t> header;
    header.reserve(signaling::kHeaderSize);
    for (int i = 0; i < static_cast<int>(signaling::kHeaderSize); ++i) {
        header.push_back(static_cast<uint8_t>(frame.at(i)));
    }
    const auto decoded = signaling::decodeHeader(header, 1024 * 1024);
    assert(decoded.has_value());
    assert(decoded->type == kMediaScreenShare);

    meeting::MediaScreenShare share;
    const QByteArray payload = frame.mid(static_cast<int>(signaling::kHeaderSize), static_cast<int>(decoded->length));
    assert(share.ParseFromArray(payload.constData(), payload.size()));
    assert(share.sharing() == expectedSharing);
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTcpServer server;
    assert(server.listen(QHostAddress::LocalHost, 0));

    signaling::SignalingClient client;
    client.connectToServer(QStringLiteral("127.0.0.1"), server.serverPort());

    assert(server.waitForNewConnection(2000));
    QTcpSocket* socket = server.nextPendingConnection();
    assert(socket != nullptr);
    assert(waitForCondition(app, [&client]() { return client.isConnected(); }, 2000));

    client.sendMediaOffer(QStringLiteral("peer-offer"), QStringLiteral("offer-sdp"), 1111, 2222);
    const QByteArray offerFrame = readFrame(app, *socket);
    assertOfferFrame(offerFrame, QStringLiteral("peer-offer"), QStringLiteral("offer-sdp"), 1111, 2222);

    client.sendMediaAnswer(QStringLiteral("peer-answer"), QStringLiteral("answer-sdp"), 3333, 4444);
    const QByteArray answerFrame = readFrame(app, *socket);
    assertAnswerFrame(answerFrame, QStringLiteral("peer-answer"), QStringLiteral("answer-sdp"), 3333, 4444);

    client.sendMediaMuteToggle(0, true);
    const QByteArray muteToggleFrame = readFrame(app, *socket);
    assertMuteToggleFrame(muteToggleFrame, 0, true);

    client.sendMediaScreenShare(true);
    const QByteArray screenShareFrame = readFrame(app, *socket);
    assertScreenShareFrame(screenShareFrame, true);

    socket->disconnectFromHost();
    socket->deleteLater();
    return 0;
}
