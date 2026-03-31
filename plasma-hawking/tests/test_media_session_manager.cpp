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

    const QString offer = sender.buildOffer(QStringLiteral("guest-1"));
    const QJsonObject offerObj = toObject(offer);
    assert(offerObj.value(QStringLiteral("kind")).toString() == QStringLiteral("offer"));
    assert(offerObj.value(QStringLiteral("local_user_id")).toString() == QStringLiteral("host-1"));
    assert(offerObj.value(QStringLiteral("peer_user_id")).toString() == QStringLiteral("guest-1"));
    assert(offerObj.value(QStringLiteral("host")).toString() == QStringLiteral("10.0.0.4"));
    assert(offerObj.value(QStringLiteral("port")).toInt() == 6200);
    assert(offerObj.value(QStringLiteral("payload")).toInt() == 111);

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
    QObject::connect(&receiver, &MediaSessionManager::remoteEndpointReady, &app,
                     [&](const QString& peerUserId, const QString& host, quint16 port, int payloadType, bool offerFlag) {
        emittedPeer = peerUserId;
        emittedHost = host;
        emittedPort = port;
        emittedPayload = payloadType;
        emittedOffer = offerFlag;
    });

    assert(receiver.handleRemoteOffer(QString(), offer));
    assert(emittedPeer == QStringLiteral("host-1"));
    assert(emittedHost == QStringLiteral("10.0.0.4"));
    assert(emittedPort == 6200);
    assert(emittedPayload == 111);
    assert(emittedOffer);

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

    QObject::connect(&sender, &MediaSessionManager::remoteEndpointReady, &app,
                     [&](const QString& peerUserId, const QString& host, quint16 port, int payloadType, bool offerFlag) {
        emittedPeer = peerUserId;
        emittedHost = host;
        emittedPort = port;
        emittedPayload = payloadType;
        emittedOffer = offerFlag;
    });

    assert(sender.handleRemoteAnswer(QString(), answer));
    assert(emittedPeer == QStringLiteral("guest-1"));
    assert(emittedHost == QStringLiteral("10.0.0.5"));
    assert(emittedPort == 6300);
    assert(emittedPayload == 111);
    assert(!emittedOffer);

    return 0;
}
