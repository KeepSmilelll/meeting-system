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
constexpr quint16 kMeetJoinRsp = 0x0204;

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

void assertTransportOfferFrame(const QByteArray& frame, const QString& expectedMeetingId) {
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
    assert(QString::fromStdString(offer.meeting_id()) == expectedMeetingId);
    assert(offer.publish_audio());
    assert(offer.publish_video());
    assert(QString::fromStdString(offer.client_ice_ufrag()) == QStringLiteral("clientUfrag"));
    assert(QString::fromStdString(offer.client_ice_pwd()) == QStringLiteral("clientPwd"));
    assert(QString::fromStdString(offer.client_dtls_fingerprint()) == QStringLiteral("AA:BB"));
    assert(offer.client_candidates_size() == 1);
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

    bool joinReceived = false;
    QString joinedMeetingId;
    QString joinedTitle;
    QString joinedSfuAddress;
    bool transportAnswerReceived = false;
    QString transportMeetingId;
    quint32 assignedAudioSsrc = 0;
    quint32 assignedVideoSsrc = 0;
    QObject::connect(&client, &signaling::SignalingClient::joinMeetingFinished, &app,
                     [&](bool success,
                         const QString& meetingId,
                         const QString& title,
                         const QString& sfuAddress,
                         const QStringList&,
                         const QString&,
                         const QString&) {
        assert(success);
        joinReceived = true;
        joinedMeetingId = meetingId;
        joinedTitle = title;
        joinedSfuAddress = sfuAddress;
    });
    QObject::connect(&client, &signaling::SignalingClient::mediaTransportAnswerReceived, &app,
                     [&](const QString& meetingId,
                         const QString& serverIceUfrag,
                         const QString& serverIcePwd,
                         const QString& serverDtlsFingerprint,
                         const QStringList& serverCandidates,
                         quint32 audioSsrc,
                         quint32 videoSsrc) {
        assert(serverIceUfrag == QStringLiteral("serverUfrag"));
        assert(serverIcePwd == QStringLiteral("serverPwd"));
        assert(serverDtlsFingerprint == QStringLiteral("CC:DD"));
        assert(serverCandidates.size() == 1);
        transportAnswerReceived = true;
        transportMeetingId = meetingId;
        assignedAudioSsrc = audioSsrc;
        assignedVideoSsrc = videoSsrc;
    });

    meeting::MeetJoinRsp joinRsp;
    joinRsp.set_success(true);
    joinRsp.set_meeting_id("meet-join");
    joinRsp.set_title("Room Title");
    joinRsp.set_sfu_address("10.0.0.8:10000");
    auto* participant = joinRsp.add_participants();
    participant->set_user_id("host-1");
    participant->set_display_name("Host");
    participant->set_role(1);
    std::string joinPayload;
    assert(joinRsp.SerializeToString(&joinPayload));
    const std::vector<uint8_t> joinFrame = signaling::encodeFrame(
        kMeetJoinRsp,
        std::vector<uint8_t>(joinPayload.begin(), joinPayload.end()));
    assert(socket->write(reinterpret_cast<const char*>(joinFrame.data()), static_cast<qint64>(joinFrame.size())) ==
           static_cast<qint64>(joinFrame.size()));
    socket->flush();
    assert(waitForCondition(app, [&joinReceived]() { return joinReceived; }, 2000));
    assert(joinedMeetingId == QStringLiteral("meet-join"));
    assert(joinedTitle == QStringLiteral("Room Title"));
    assert(joinedSfuAddress == QStringLiteral("10.0.0.8:10000"));

    client.sendTransportOffer(QStringLiteral("meet-join"),
                              true,
                              true,
                              QStringLiteral("clientUfrag"),
                              QStringLiteral("clientPwd"),
                              QStringLiteral("AA:BB"),
                              QStringList{QStringLiteral("candidate:1 1 udp 2130706431 127.0.0.1 5004 typ host")});
    const QByteArray offerFrame = readFrame(app, *socket);
    assertTransportOfferFrame(offerFrame, QStringLiteral("meet-join"));

    meeting::MediaAnswer answer;
    answer.set_meeting_id("meet-join");
    answer.set_server_ice_ufrag("serverUfrag");
    answer.set_server_ice_pwd("serverPwd");
    answer.set_server_dtls_fingerprint("CC:DD");
    answer.add_server_candidates("candidate:2 1 udp 2130706431 10.0.0.8 10000 typ host");
    answer.set_assigned_audio_ssrc(3333);
    answer.set_assigned_video_ssrc(4444);
    std::string answerPayload;
    assert(answer.SerializeToString(&answerPayload));
    const std::vector<uint8_t> answerFrame = signaling::encodeFrame(
        kMediaAnswer,
        std::vector<uint8_t>(answerPayload.begin(), answerPayload.end()));
    assert(socket->write(reinterpret_cast<const char*>(answerFrame.data()), static_cast<qint64>(answerFrame.size())) ==
           static_cast<qint64>(answerFrame.size()));
    socket->flush();
    assert(waitForCondition(app, [&transportAnswerReceived]() { return transportAnswerReceived; }, 2000));
    assert(transportMeetingId == QStringLiteral("meet-join"));
    assert(assignedAudioSsrc == 3333);
    assert(assignedVideoSsrc == 4444);

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
