#include <cassert>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "../src/app/MediaSessionManager.h"

namespace {

QJsonObject toObject(const QString& json) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    assert(error.error == QJsonParseError::NoError);
    assert(doc.isObject());
    return doc.object();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    MediaSessionManager sender;
    sender.setLocalUserId(QStringLiteral("host-1"));
    sender.setMeetingId(QStringLiteral("meet-1"));
    sender.setLocalHost(QStringLiteral("10.0.0.4"));
    sender.setLocalPort(6200);
    sender.setPayloadType(111);
    sender.setLocalVideoPort(7200);
    sender.setVideoPayloadType(97);
    sender.setLocalVideoSsrc(5555);

    const QString offer = sender.buildOffer(QStringLiteral("guest-1"));
    const QJsonObject offerObj = toObject(offer);
    assert(offerObj.value(QStringLiteral("kind")).toString() == QStringLiteral("offer"));
    assert(offerObj.value(QStringLiteral("local_user_id")).toString() == QStringLiteral("host-1"));
    assert(offerObj.value(QStringLiteral("peer_user_id")).toString() == QStringLiteral("guest-1"));
    assert(offerObj.value(QStringLiteral("host")).toString() == QStringLiteral("10.0.0.4"));
    assert(offerObj.value(QStringLiteral("port")).toInt() == 6200);
    assert(offerObj.value(QStringLiteral("payload")).toInt() == 111);
    const QJsonObject audioObj = offerObj.value(QStringLiteral("audio")).toObject();
    assert(audioObj.value(QStringLiteral("port")).toInt() == 6200);
    assert(audioObj.value(QStringLiteral("payload")).toInt() == 111);
    const QJsonObject videoObj = offerObj.value(QStringLiteral("video")).toObject();
    assert(videoObj.value(QStringLiteral("port")).toInt() == 7200);
    assert(videoObj.value(QStringLiteral("payload")).toInt() == 97);
    assert(offerObj.value(QStringLiteral("video_ssrc")).toInt() == 5555);

    MediaSessionManager receiver;
    receiver.setLocalUserId(QStringLiteral("guest-1"));
    receiver.setMeetingId(QStringLiteral("meet-1"));
    receiver.setLocalHost(QStringLiteral("10.0.0.5"));
    receiver.setLocalPort(6300);

    QString emittedPeer;
    QString emittedHost;
    quint16 emittedPort = 0;
    int emittedPayload = 0;
    bool emittedOffer = false;
    QString emittedVideoHost;
    quint16 emittedVideoPort = 0;
    int emittedVideoPayload = 0;
    QObject::connect(&receiver, &MediaSessionManager::remoteEndpointReady, &app,
                     [&](const QString& peerUserId, const QString& host, quint16 port, int payloadType, bool offerFlag) {
        emittedPeer = peerUserId;
        emittedHost = host;
        emittedPort = port;
        emittedPayload = payloadType;
        emittedOffer = offerFlag;
    });
    QObject::connect(&receiver, &MediaSessionManager::remoteVideoEndpointReady, &app,
                     [&](const QString&, const QString& host, quint16 port, int payloadType, bool) {
        emittedVideoHost = host;
        emittedVideoPort = port;
        emittedVideoPayload = payloadType;
    });

    assert(receiver.handleRemoteOffer(QString(), offer));
    assert(emittedPeer == QStringLiteral("host-1"));
    assert(emittedHost == QStringLiteral("10.0.0.4"));
    assert(emittedPort == 6200);
    assert(emittedPayload == 111);
    assert(emittedOffer);
    assert(emittedVideoHost == QStringLiteral("10.0.0.4"));
    assert(emittedVideoPort == 7200);
    assert(emittedVideoPayload == 97);

    const QString answer = receiver.buildAnswer(QStringLiteral("host-1"));
    const QJsonObject answerObj = toObject(answer);
    assert(answerObj.value(QStringLiteral("kind")).toString() == QStringLiteral("answer"));
    assert(answerObj.value(QStringLiteral("local_user_id")).toString() == QStringLiteral("guest-1"));
    assert(answerObj.value(QStringLiteral("peer_user_id")).toString() == QStringLiteral("host-1"));

    emittedPeer.clear();
    emittedHost.clear();
    emittedPort = 0;
    emittedPayload = 0;
    emittedOffer = true;
    emittedVideoHost.clear();
    emittedVideoPort = 0;
    emittedVideoPayload = 0;

    QObject::connect(&sender, &MediaSessionManager::remoteEndpointReady, &app,
                     [&](const QString& peerUserId, const QString& host, quint16 port, int payloadType, bool offerFlag) {
        emittedPeer = peerUserId;
        emittedHost = host;
        emittedPort = port;
        emittedPayload = payloadType;
        emittedOffer = offerFlag;
    });
    QObject::connect(&sender, &MediaSessionManager::remoteVideoEndpointReady, &app,
                     [&](const QString&, const QString& host, quint16 port, int payloadType, bool) {
        emittedVideoHost = host;
        emittedVideoPort = port;
        emittedVideoPayload = payloadType;
    });

    assert(sender.handleRemoteAnswer(QString(), answer));
    assert(emittedPeer == QStringLiteral("guest-1"));
    assert(emittedHost == QStringLiteral("10.0.0.5"));
    assert(emittedPort == 6300);
    assert(emittedPayload == 111);
    assert(!emittedOffer);
    assert(emittedVideoHost == QStringLiteral("10.0.0.5"));
    assert(emittedVideoPort == 5006);
    assert(emittedVideoPayload == 97);

    MediaSessionManager videoOnlySender;
    videoOnlySender.setLocalUserId(QStringLiteral("viewer-1"));
    videoOnlySender.setMeetingId(QStringLiteral("meet-1"));
    videoOnlySender.setLocalHost(QStringLiteral("10.0.0.6"));
    videoOnlySender.setAudioNegotiationEnabled(false);
    videoOnlySender.setLocalVideoPort(7400);
    videoOnlySender.setVideoPayloadType(97);

    const QString videoOnlyOffer = videoOnlySender.buildOffer(QStringLiteral("sharer-1"));
    const QJsonObject videoOnlyObj = toObject(videoOnlyOffer);
    assert(!videoOnlyObj.contains(QStringLiteral("audio")));
    assert(videoOnlyObj.value(QStringLiteral("peer_user_id")).toString() == QStringLiteral("sharer-1"));
    assert(videoOnlyObj.value(QStringLiteral("video")).toObject().value(QStringLiteral("port")).toInt() == 7400);

    MediaSessionManager videoOnlyReceiver;
    videoOnlyReceiver.setLocalUserId(QStringLiteral("sharer-1"));
    videoOnlyReceiver.setMeetingId(QStringLiteral("meet-1"));

    bool audioSignalReceived = false;
    emittedPeer.clear();
    emittedVideoHost.clear();
    emittedVideoPort = 0;
    QObject::connect(&videoOnlyReceiver, &MediaSessionManager::remoteEndpointReady, &app,
                     [&](const QString&, const QString&, quint16, int, bool) {
        audioSignalReceived = true;
    });
    QObject::connect(&videoOnlyReceiver, &MediaSessionManager::remoteVideoEndpointReady, &app,
                     [&](const QString& peerUserId, const QString& host, quint16 port, int payloadType, bool offerFlag) {
        emittedPeer = peerUserId;
        emittedVideoHost = host;
        emittedVideoPort = port;
        emittedVideoPayload = payloadType;
        emittedOffer = offerFlag;
    });

    assert(videoOnlyReceiver.handleRemoteOffer(QString(), videoOnlyOffer));
    assert(!audioSignalReceived);
    assert(emittedPeer == QStringLiteral("viewer-1"));
    assert(emittedVideoHost == QStringLiteral("10.0.0.6"));
    assert(emittedVideoPort == 7400);
    assert(emittedVideoPayload == 97);
    assert(emittedOffer);

    return 0;
}
