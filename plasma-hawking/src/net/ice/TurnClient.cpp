#include "TurnClient.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMessageAuthenticationCode>
#include <QtEndian>

#include <cstring>

namespace ice {
namespace {

constexpr quint16 kAllocateRequest = 0x0003;
constexpr quint16 kAllocateSuccess = 0x0103;
constexpr quint16 kAllocateError = 0x0113;
constexpr quint16 kCreatePermissionRequest = 0x0008;
constexpr quint16 kCreatePermissionSuccess = 0x0108;
constexpr quint16 kCreatePermissionError = 0x0118;
constexpr quint16 kSendIndication = 0x0016;
constexpr quint16 kDataIndication = 0x0017;
constexpr quint32 kMagicCookie = 0x2112A442U;
constexpr int kHeaderSize = 20;

constexpr quint16 kAttrUsername = 0x0006;
constexpr quint16 kAttrMessageIntegrity = 0x0008;
constexpr quint16 kAttrErrorCode = 0x0009;
constexpr quint16 kAttrRealm = 0x0014;
constexpr quint16 kAttrNonce = 0x0015;
constexpr quint16 kAttrXorRelayedAddress = 0x0016;
constexpr quint16 kAttrRequestedTransport = 0x0019;
constexpr quint16 kAttrXorPeerAddress = 0x0012;
constexpr quint16 kAttrData = 0x0013;
constexpr quint16 kAttrXorMappedAddress = 0x0020;

quint16 readU16(const char* data) {
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(data));
}

quint32 readU32(const char* data) {
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data));
}

void writeU16(char* data, quint16 value) {
    qToBigEndian(value, reinterpret_cast<uchar*>(data));
}

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

int paddedLength(int length) {
    return (length + 3) & ~3;
}

void updateLength(QByteArray* packet) {
    if (packet == nullptr || packet->size() < kHeaderSize) {
        return;
    }
    writeU16(packet->data() + 2, static_cast<quint16>(packet->size() - kHeaderSize));
}

void appendHeader(QByteArray* packet, quint16 messageType, const QByteArray& transactionId) {
    appendU16(packet, messageType);
    appendU16(packet, 0);
    appendU32(packet, kMagicCookie);
    packet->append(transactionId);
}

void appendAttribute(QByteArray* packet, quint16 type, const QByteArray& value) {
    appendU16(packet, type);
    appendU16(packet, static_cast<quint16>(value.size()));
    packet->append(value);
    const int pad = paddedLength(value.size()) - value.size();
    if (pad > 0) {
        packet->append(QByteArray(pad, '\0'));
    }
    updateLength(packet);
}

QByteArray addressAttributeValue(const QHostAddress& address, quint16 port) {
    if (address.protocol() != QAbstractSocket::IPv4Protocol || port == 0) {
        return {};
    }
    QByteArray value;
    value.resize(8);
    value[0] = 0;
    value[1] = 0x01;
    writeU16(value.data() + 2, static_cast<quint16>(port ^ (kMagicCookie >> 16U)));
    qToBigEndian(address.toIPv4Address() ^ kMagicCookie, reinterpret_cast<uchar*>(value.data() + 4));
    return value;
}

bool parseAddressAttribute(const char* value, int length, StunEndpoint* endpoint) {
    if (endpoint == nullptr || length < 8 || static_cast<quint8>(value[1]) != 0x01U) {
        return false;
    }
    const quint16 port = static_cast<quint16>(readU16(value + 2) ^ (kMagicCookie >> 16U));
    const quint32 address = readU32(value + 4) ^ kMagicCookie;
    endpoint->address = QHostAddress(address);
    endpoint->port = port;
    return endpoint->isValid();
}

QByteArray longTermKey(const QString& username, const QString& realm, const QString& credential) {
    return QCryptographicHash::hash(
        QStringLiteral("%1:%2:%3").arg(username, realm, credential).toUtf8(),
        QCryptographicHash::Md5);
}

void appendMessageIntegrity(QByteArray* packet,
                            const QString& username,
                            const QString& realm,
                            const QString& credential) {
    const int attrOffset = packet->size();
    appendU16(packet, kAttrMessageIntegrity);
    appendU16(packet, 20);
    packet->append(QByteArray(20, '\0'));
    updateLength(packet);

    const QByteArray key = longTermKey(username, realm, credential);
    // STUN MESSAGE-INTEGRITY authenticates the header plus attributes before
    // this attribute, while the header length already includes this attribute.
    const QByteArray authenticated = packet->left(attrOffset);
    const QByteArray hmac = QMessageAuthenticationCode::hash(authenticated, key, QCryptographicHash::Sha1);
    std::memcpy(packet->data() + attrOffset + 4, hmac.constData(), 20);
}

QString parseErrorCode(const char* value, int length) {
    if (length < 4) {
        return QStringLiteral("TURN error response");
    }
    const int code = (static_cast<quint8>(value[2]) * 100) + static_cast<quint8>(value[3]);
    const QString reason = length > 4 ? QString::fromUtf8(value + 4, length - 4) : QString();
    return reason.isEmpty() ? QStringLiteral("TURN error %1").arg(code)
                            : QStringLiteral("TURN error %1: %2").arg(code).arg(reason);
}

bool parseAttributes(const QByteArray& datagram,
                     QHash<quint16, QByteArray>* attrs,
                     QString* error) {
    if (datagram.size() < kHeaderSize || readU32(datagram.constData() + 4) != kMagicCookie) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid TURN/STUN header");
        }
        return false;
    }
    const int messageLength = readU16(datagram.constData() + 2);
    if (datagram.size() < kHeaderSize + messageLength) {
        if (error != nullptr) {
            *error = QStringLiteral("truncated TURN/STUN message");
        }
        return false;
    }
    int offset = kHeaderSize;
    const int end = kHeaderSize + messageLength;
    while (offset + 4 <= end) {
        const quint16 attrType = readU16(datagram.constData() + offset);
        const quint16 attrLength = readU16(datagram.constData() + offset + 2);
        offset += 4;
        if (offset + attrLength > end) {
            break;
        }
        if (attrs != nullptr) {
            attrs->insert(attrType, QByteArray(datagram.constData() + offset, attrLength));
        }
        offset += paddedLength(attrLength);
    }
    return true;
}

bool transactionMatches(const QByteArray& datagram, const QByteArray& expectedTransactionId) {
    return datagram.size() >= kHeaderSize &&
           expectedTransactionId.size() == 12 &&
           QByteArrayView(datagram.constData() + 8, 12) == QByteArrayView(expectedTransactionId);
}

}  // namespace

bool TurnClient::parseTurnServerUrl(const QString& url, TurnServerConfig* out) {
    if (out == nullptr) {
        return false;
    }
    QString normalized = url.trimmed();
    if (normalized.startsWith(QStringLiteral("turn:"), Qt::CaseInsensitive)) {
        normalized = normalized.mid(5);
    } else if (normalized.startsWith(QStringLiteral("turns:"), Qt::CaseInsensitive)) {
        normalized = normalized.mid(6);
    } else {
        return false;
    }
    const int queryIndex = normalized.indexOf(QLatin1Char('?'));
    if (queryIndex >= 0) {
        normalized = normalized.left(queryIndex);
    }
    const int colonIndex = normalized.lastIndexOf(QLatin1Char(':'));
    bool ok = false;
    const quint16 parsedPort = colonIndex >= 0 ? normalized.mid(colonIndex + 1).toUShort(&ok) : 3478;
    const QString parsedHost = colonIndex >= 0 ? normalized.left(colonIndex) : normalized;
    if (parsedHost.trimmed().isEmpty() || parsedPort == 0 || (colonIndex >= 0 && !ok)) {
        return false;
    }
    out->host = parsedHost.trimmed();
    out->port = parsedPort;
    return true;
}

QByteArray TurnClient::buildAllocateRequest(const QByteArray& transactionId) {
    QByteArray packet;
    if (transactionId.size() != 12) {
        return packet;
    }
    appendHeader(&packet, kAllocateRequest, transactionId);
    QByteArray transport;
    transport.resize(4);
    transport[0] = 17; // UDP
    appendAttribute(&packet, kAttrRequestedTransport, transport);
    return packet;
}

QByteArray TurnClient::buildAuthenticatedAllocateRequest(const QByteArray& transactionId,
                                                         const QString& username,
                                                         const QString& realm,
                                                         const QString& nonce,
                                                         const QString& credential) {
    QByteArray packet = buildAllocateRequest(transactionId);
    if (packet.isEmpty() || username.isEmpty() || realm.isEmpty() || nonce.isEmpty()) {
        return {};
    }
    appendAttribute(&packet, kAttrUsername, username.toUtf8());
    appendAttribute(&packet, kAttrRealm, realm.toUtf8());
    appendAttribute(&packet, kAttrNonce, nonce.toUtf8());
    appendMessageIntegrity(&packet, username, realm, credential);
    return packet;
}

QByteArray TurnClient::buildCreatePermissionRequest(const QByteArray& transactionId,
                                                    const QHostAddress& peerAddress,
                                                    const QString& username,
                                                    const QString& realm,
                                                    const QString& nonce,
                                                    const QString& credential) {
    QByteArray packet;
    if (transactionId.size() != 12 || peerAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        return packet;
    }
    appendHeader(&packet, kCreatePermissionRequest, transactionId);
    appendAttribute(&packet, kAttrXorPeerAddress, addressAttributeValue(peerAddress, 1));
    appendAttribute(&packet, kAttrUsername, username.toUtf8());
    appendAttribute(&packet, kAttrRealm, realm.toUtf8());
    appendAttribute(&packet, kAttrNonce, nonce.toUtf8());
    appendMessageIntegrity(&packet, username, realm, credential);
    return packet;
}

QByteArray TurnClient::buildSendIndication(const QHostAddress& peerAddress,
                                           quint16 peerPort,
                                           const QByteArray& payload) {
    const QByteArray transactionId = StunClient::makeTransactionId();
    QByteArray packet;
    if (peerAddress.protocol() != QAbstractSocket::IPv4Protocol || peerPort == 0 || payload.isEmpty()) {
        return packet;
    }
    appendHeader(&packet, kSendIndication, transactionId);
    appendAttribute(&packet, kAttrXorPeerAddress, addressAttributeValue(peerAddress, peerPort));
    appendAttribute(&packet, kAttrData, payload);
    return packet;
}

bool TurnClient::parseAllocateResponse(const QByteArray& datagram,
                                       const QByteArray& expectedTransactionId,
                                       TurnAllocateResult* result) {
    if (result == nullptr) {
        return false;
    }
    *result = {};
    if (!transactionMatches(datagram, expectedTransactionId)) {
        result->error = QStringLiteral("TURN transaction id mismatch");
        return false;
    }
    QHash<quint16, QByteArray> attrs;
    if (!parseAttributes(datagram, &attrs, &result->error)) {
        return false;
    }
    const quint16 messageType = readU16(datagram.constData());
    if (messageType == kAllocateError) {
        if (attrs.contains(kAttrRealm)) {
            result->realm = QString::fromUtf8(attrs.value(kAttrRealm));
        }
        if (attrs.contains(kAttrNonce)) {
            result->nonce = QString::fromUtf8(attrs.value(kAttrNonce));
        }
        if (attrs.contains(kAttrErrorCode)) {
            result->error = parseErrorCode(attrs.value(kAttrErrorCode).constData(), attrs.value(kAttrErrorCode).size());
        }
        return false;
    }
    if (messageType != kAllocateSuccess) {
        result->error = QStringLiteral("unexpected TURN Allocate response");
        return false;
    }

    StunEndpoint relay{};
    StunEndpoint mapped{};
    const QByteArray relayAttr = attrs.value(kAttrXorRelayedAddress);
    const QByteArray mappedAttr = attrs.value(kAttrXorMappedAddress);
    if (!parseAddressAttribute(relayAttr.constData(), relayAttr.size(), &relay)) {
        result->error = QStringLiteral("TURN Allocate missing relayed address");
        return false;
    }
    (void)parseAddressAttribute(mappedAttr.constData(), mappedAttr.size(), &mapped);
    result->success = true;
    result->relayedAddress = relay;
    result->mappedAddress = mapped;
    return true;
}

bool TurnClient::parseCreatePermissionResponse(const QByteArray& datagram,
                                               const QByteArray& expectedTransactionId,
                                               QString* error) {
    if (!transactionMatches(datagram, expectedTransactionId)) {
        if (error != nullptr) {
            *error = QStringLiteral("TURN CreatePermission transaction id mismatch");
        }
        return false;
    }
    QHash<quint16, QByteArray> attrs;
    QString parseError;
    if (!parseAttributes(datagram, &attrs, &parseError)) {
        if (error != nullptr) {
            *error = parseError;
        }
        return false;
    }
    const quint16 messageType = readU16(datagram.constData());
    if (messageType == kCreatePermissionSuccess) {
        return true;
    }
    if (messageType == kCreatePermissionError && attrs.contains(kAttrErrorCode)) {
        if (error != nullptr) {
            *error = parseErrorCode(attrs.value(kAttrErrorCode).constData(), attrs.value(kAttrErrorCode).size());
        }
        return false;
    }
    if (error != nullptr) {
        *error = QStringLiteral("unexpected TURN CreatePermission response");
    }
    return false;
}

bool TurnClient::parseDataIndication(const QByteArray& datagram,
                                     StunEndpoint* peer,
                                     QByteArray* payload,
                                     QString* error) {
    if (datagram.size() < kHeaderSize || readU16(datagram.constData()) != kDataIndication) {
        if (error != nullptr) {
            *error = QStringLiteral("not a TURN Data indication");
        }
        return false;
    }
    QHash<quint16, QByteArray> attrs;
    if (!parseAttributes(datagram, &attrs, error)) {
        return false;
    }
    const QByteArray peerAttr = attrs.value(kAttrXorPeerAddress);
    StunEndpoint parsedPeer{};
    if (!parseAddressAttribute(peerAttr.constData(), peerAttr.size(), &parsedPeer) || !attrs.contains(kAttrData)) {
        if (error != nullptr) {
            *error = QStringLiteral("TURN Data indication missing peer or data");
        }
        return false;
    }
    if (peer != nullptr) {
        *peer = parsedPeer;
    }
    if (payload != nullptr) {
        *payload = attrs.value(kAttrData);
    }
    return true;
}

QString TurnClient::makeRelayCandidate(const QHostAddress& host, quint16 port, quint32 priority) {
    if (host.isNull() || port == 0) {
        return {};
    }
    return QStringLiteral("candidate:3 1 udp %1 %2 %3 typ relay").arg(priority).arg(host.toString()).arg(port);
}

}  // namespace ice
