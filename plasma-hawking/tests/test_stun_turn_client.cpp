#include <cassert>
#include <iostream>
#include <thread>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QHostAddress>
#include <QMessageAuthenticationCode>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QtEndian>

#include "net/ice/StunClient.h"
#include "net/ice/TurnClient.h"
#include "net/media/UdpPeerSocket.h"

namespace {

constexpr quint32 kMagicCookie = 0x2112A442U;

void appendU16(QByteArray* out, quint16 value) {
    char bytes[2];
    qToBigEndian(value, reinterpret_cast<uchar*>(bytes));
    out->append(bytes, 2);
}

void appendU32(QByteArray* out, quint32 value) {
    char bytes[4];
    qToBigEndian(value, reinterpret_cast<uchar*>(bytes));
    out->append(bytes, 4);
}

void updateLength(QByteArray* packet) {
    qToBigEndian(static_cast<quint16>(packet->size() - 20), reinterpret_cast<uchar*>(packet->data() + 2));
}

void appendHeader(QByteArray* packet, quint16 type, const QByteArray& transactionId) {
    appendU16(packet, type);
    appendU16(packet, 0);
    appendU32(packet, kMagicCookie);
    packet->append(transactionId);
}

void appendAttr(QByteArray* packet, quint16 type, const QByteArray& value) {
    appendU16(packet, type);
    appendU16(packet, static_cast<quint16>(value.size()));
    packet->append(value);
    const int pad = ((value.size() + 3) & ~3) - value.size();
    if (pad > 0) {
        packet->append(QByteArray(pad, '\0'));
    }
    updateLength(packet);
}

QByteArray xorAddressValue(const QHostAddress& address, quint16 port) {
    QByteArray value;
    value.resize(8);
    value[0] = 0;
    value[1] = 1;
    qToBigEndian(static_cast<quint16>(port ^ (kMagicCookie >> 16U)), reinterpret_cast<uchar*>(value.data() + 2));
    qToBigEndian(address.toIPv4Address() ^ kMagicCookie, reinterpret_cast<uchar*>(value.data() + 4));
    return value;
}

QByteArray errorCodeValue(int code, const QByteArray& reason) {
    QByteArray value;
    value.resize(4);
    value[0] = 0;
    value[1] = 0;
    value[2] = static_cast<char>(code / 100);
    value[3] = static_cast<char>(code % 100);
    value.append(reason);
    return value;
}

quint16 readU16(const char* data) {
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(data));
}

QByteArray transactionIdFrom(const QByteArray& datagram) {
    assert(datagram.size() >= 20);
    return QByteArray(datagram.constData() + 8, 12);
}

QByteArray attrValue(const QByteArray& datagram, quint16 attrType) {
    int offset = 20;
    const int end = 20 + readU16(datagram.constData() + 2);
    while (offset + 4 <= end && offset + 4 <= datagram.size()) {
        const quint16 type = readU16(datagram.constData() + offset);
        const quint16 length = readU16(datagram.constData() + offset + 2);
        offset += 4;
        if (offset + length > datagram.size()) {
            return {};
        }
        if (type == attrType) {
            return QByteArray(datagram.constData() + offset, length);
        }
        offset += (length + 3) & ~3;
    }
    return {};
}

int attrOffset(const QByteArray& datagram, quint16 attrType) {
    int offset = 20;
    const int end = 20 + readU16(datagram.constData() + 2);
    while (offset + 4 <= end && offset + 4 <= datagram.size()) {
        const int attrStart = offset;
        const quint16 type = readU16(datagram.constData() + offset);
        const quint16 length = readU16(datagram.constData() + offset + 2);
        offset += 4;
        if (offset + length > datagram.size()) {
            return -1;
        }
        if (type == attrType) {
            return attrStart;
        }
        offset += (length + 3) & ~3;
    }
    return -1;
}

QByteArray waitDatagram(QCoreApplication& app, QUdpSocket& socket, QHostAddress* sender, quint16* senderPort) {
    for (int i = 0; i < 200; ++i) {
        app.processEvents(QEventLoop::AllEvents, 10);
        if (socket.waitForReadyRead(10)) {
            const QNetworkDatagram datagram = socket.receiveDatagram();
            if (sender != nullptr) {
                *sender = datagram.senderAddress();
            }
            if (senderPort != nullptr) {
                *senderPort = static_cast<quint16>(datagram.senderPort());
            }
            return datagram.data();
        }
    }
    return {};
}

void testStunBindingParse() {
    const QByteArray tx = QByteArrayLiteral("abcdefghijkl");
    const QByteArray request = ice::StunClient::buildBindingRequest(tx);
    assert(request.size() == 20);

    QByteArray response;
    appendHeader(&response, 0x0101, tx);
    appendAttr(&response, 0x0020, xorAddressValue(QHostAddress(QStringLiteral("203.0.113.7")), 54321));

    ice::StunEndpoint mapped;
    QString error;
    assert(ice::StunClient::parseBindingResponse(response, tx, &mapped, &error));
    assert(mapped.address.toString() == QStringLiteral("203.0.113.7"));
    assert(mapped.port == 54321);
    assert(!ice::StunClient::parseBindingResponse(response, QByteArrayLiteral("badbadbadbad"), &mapped, &error));
}

void testTurnAllocateParse() {
    const QByteArray tx = QByteArrayLiteral("mnopqrstuvwx");
    const QByteArray unauth = ice::TurnClient::buildAllocateRequest(tx);
    assert(unauth.size() >= 28);

    QByteArray challenge;
    appendHeader(&challenge, 0x0113, tx);
    appendAttr(&challenge, 0x0009, errorCodeValue(401, QByteArrayLiteral("Unauthorized")));
    appendAttr(&challenge, 0x0014, QByteArrayLiteral("example.org"));
    appendAttr(&challenge, 0x0015, QByteArrayLiteral("nonce-1"));

    ice::TurnAllocateResult challenged;
    assert(!ice::TurnClient::parseAllocateResponse(challenge, tx, &challenged));
    assert(challenged.realm == QStringLiteral("example.org"));
    assert(challenged.nonce == QStringLiteral("nonce-1"));

    const QByteArray auth = ice::TurnClient::buildAuthenticatedAllocateRequest(
        tx,
        QStringLiteral("user"),
        challenged.realm,
        challenged.nonce,
        QStringLiteral("pass"));
    assert(auth.size() > unauth.size());
    const int integrityOffset = attrOffset(auth, 0x0008);
    assert(integrityOffset > 20);
    const QByteArray longTermKey = QCryptographicHash::hash(
        QByteArrayLiteral("user:example.org:pass"),
        QCryptographicHash::Md5);
    assert(attrValue(auth, 0x0008) == QMessageAuthenticationCode::hash(
                                         auth.left(integrityOffset),
                                         longTermKey,
                                         QCryptographicHash::Sha1));

    QByteArray success;
    appendHeader(&success, 0x0103, tx);
    appendAttr(&success, 0x0016, xorAddressValue(QHostAddress(QStringLiteral("198.51.100.9")), 62000));
    appendAttr(&success, 0x0020, xorAddressValue(QHostAddress(QStringLiteral("203.0.113.8")), 52000));

    ice::TurnAllocateResult allocated;
    assert(ice::TurnClient::parseAllocateResponse(success, tx, &allocated));
    assert(allocated.relayedAddress.address.toString() == QStringLiteral("198.51.100.9"));
    assert(allocated.relayedAddress.port == 62000);
    assert(allocated.mappedAddress.address.toString() == QStringLiteral("203.0.113.8"));
}

void testTurnPermissionAndData() {
    const QByteArray tx = QByteArrayLiteral("yzabcdefghij");
    const QByteArray permission = ice::TurnClient::buildCreatePermissionRequest(
        tx,
        QHostAddress(QStringLiteral("10.0.0.8")),
        QStringLiteral("user"),
        QStringLiteral("realm"),
        QStringLiteral("nonce"),
        QStringLiteral("pass"));
    assert(permission.size() > 20);

    QByteArray permissionSuccess;
    appendHeader(&permissionSuccess, 0x0108, tx);
    QString permissionError;
    assert(ice::TurnClient::parseCreatePermissionResponse(permissionSuccess, tx, &permissionError));

    const QByteArray payload = QByteArrayLiteral("dtls-or-rtp");
    const QByteArray send = ice::TurnClient::buildSendIndication(QHostAddress(QStringLiteral("10.0.0.8")), 10000, payload);
    assert(send.size() > payload.size());

    QByteArray data;
    appendHeader(&data, 0x0017, QByteArrayLiteral("klmnopqrstuv"));
    appendAttr(&data, 0x0012, xorAddressValue(QHostAddress(QStringLiteral("10.0.0.8")), 10000));
    appendAttr(&data, 0x0013, payload);

    ice::StunEndpoint peer;
    QByteArray parsedPayload;
    QString error;
    assert(ice::TurnClient::parseDataIndication(data, &peer, &parsedPayload, &error));
    assert(peer.address.toString() == QStringLiteral("10.0.0.8"));
    assert(peer.port == 10000);
    assert(parsedPayload == payload);
}

void testUdpPeerSocketTurnRelay(QCoreApplication& app) {
    QUdpSocket turnServer;
    assert(turnServer.bind(QHostAddress::LocalHost, 0));

    media::UdpPeerSocket socket;
    std::string openError;
    assert(socket.open("127.0.0.1", 0, &openError));
    assert(socket.setPeer("127.0.0.1", 10000));

    bool configured = false;
    std::string relayError;
    std::thread configureThread([&]() {
        configured = socket.configureTurnRelay("127.0.0.1",
                                               static_cast<quint16>(turnServer.localPort()),
                                               "user",
                                               "pass",
                                               "127.0.0.1",
                                               10000,
                                               &relayError);
    });

    QHostAddress clientAddress;
    quint16 clientPort = 0;
    QByteArray request = waitDatagram(app, turnServer, &clientAddress, &clientPort);
    assert(readU16(request.constData()) == 0x0003);
    QByteArray response;
    appendHeader(&response, 0x0113, transactionIdFrom(request));
    appendAttr(&response, 0x0009, errorCodeValue(401, QByteArrayLiteral("Unauthorized")));
    appendAttr(&response, 0x0014, QByteArrayLiteral("realm"));
    appendAttr(&response, 0x0015, QByteArrayLiteral("nonce"));
    assert(turnServer.writeDatagram(response, clientAddress, clientPort) == response.size());

    request = waitDatagram(app, turnServer, &clientAddress, &clientPort);
    assert(readU16(request.constData()) == 0x0003);
    const int allocateIntegrityOffset = attrOffset(request, 0x0008);
    assert(allocateIntegrityOffset > 20);
    const QByteArray allocateLongTermKey = QCryptographicHash::hash(
        QByteArrayLiteral("user:realm:pass"),
        QCryptographicHash::Md5);
    assert(attrValue(request, 0x0008) == QMessageAuthenticationCode::hash(
                                           request.left(allocateIntegrityOffset),
                                           allocateLongTermKey,
                                           QCryptographicHash::Sha1));
    response.clear();
    appendHeader(&response, 0x0103, transactionIdFrom(request));
    appendAttr(&response, 0x0016, xorAddressValue(QHostAddress(QStringLiteral("127.0.0.1")), 62000));
    appendAttr(&response, 0x0020, xorAddressValue(clientAddress, clientPort));
    assert(turnServer.writeDatagram(response, clientAddress, clientPort) == response.size());

    request = waitDatagram(app, turnServer, &clientAddress, &clientPort);
    assert(readU16(request.constData()) == 0x0008);
    response.clear();
    appendHeader(&response, 0x0108, transactionIdFrom(request));
    assert(turnServer.writeDatagram(response, clientAddress, clientPort) == response.size());

    configureThread.join();
    assert(configured);
    assert(relayError.empty());

    const QByteArray outbound = QByteArrayLiteral("srtp-packet");
    assert(socket.sendToPeer(reinterpret_cast<const uint8_t*>(outbound.constData()),
                             static_cast<std::size_t>(outbound.size())) == outbound.size());
    const QByteArray sendIndication = waitDatagram(app, turnServer, &clientAddress, &clientPort);
    assert(readU16(sendIndication.constData()) == 0x0016);
    assert(attrValue(sendIndication, 0x0013) == outbound);

    QByteArray dataIndication;
    appendHeader(&dataIndication, 0x0017, QByteArrayLiteral("abcdefghijkl"));
    appendAttr(&dataIndication, 0x0012, xorAddressValue(QHostAddress(QStringLiteral("127.0.0.1")), 10000));
    appendAttr(&dataIndication, 0x0013, QByteArrayLiteral("remote-srtp"));
    assert(turnServer.writeDatagram(dataIndication, clientAddress, clientPort) == dataIndication.size());
    assert(socket.waitForReadable(1000) > 0);
    uint8_t buffer[64]{};
    media::UdpEndpoint from;
    const int received = socket.recvFrom(buffer, sizeof(buffer), from);
    assert(received == QByteArrayLiteral("remote-srtp").size());
    assert(QByteArray(reinterpret_cast<const char*>(buffer), received) == QByteArrayLiteral("remote-srtp"));
    assert(from.address == QHostAddress(QStringLiteral("127.0.0.1")));
    assert(from.port == 10000);
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    Q_UNUSED(app)

    testStunBindingParse();
    testTurnAllocateParse();
    testTurnPermissionAndData();
    testUdpPeerSocketTurnRelay(app);

    std::cout << "test_stun_turn_client passed" << std::endl;
    return 0;
}
