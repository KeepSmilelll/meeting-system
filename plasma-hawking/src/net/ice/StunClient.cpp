#include "StunClient.h"

#include <QRandomGenerator>
#include <QtEndian>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QUdpSocket>

#include <algorithm>

namespace ice {
namespace {

constexpr quint16 kStunBindingRequest = 0x0001;
constexpr quint16 kStunBindingSuccess = 0x0101;
constexpr quint32 kMagicCookie = 0x2112A442U;
constexpr int kHeaderSize = 20;
constexpr quint16 kAttrMappedAddress = 0x0001;
constexpr quint16 kAttrXorMappedAddress = 0x0020;

quint16 readU16(const char* data) {
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(data));
}

quint32 readU32(const char* data) {
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data));
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

bool parseAddressAttribute(quint16 attrType, const char* value, int length, StunEndpoint* endpoint) {
    if (endpoint == nullptr || length < 8 || static_cast<quint8>(value[1]) != 0x01U) {
        return false;
    }

    quint16 port = readU16(value + 2);
    quint32 address = readU32(value + 4);
    if (attrType == kAttrXorMappedAddress) {
        port ^= static_cast<quint16>(kMagicCookie >> 16U);
        address ^= kMagicCookie;
    }

    endpoint->address = QHostAddress(address);
    endpoint->port = port;
    return endpoint->isValid();
}

}  // namespace

StunClient::StunClient() = default;

void StunClient::setTimeoutMs(int timeoutMs) {
    m_timeoutMs = std::clamp(timeoutMs, 50, 5000);
}

void StunClient::setRetryCount(int retryCount) {
    m_retryCount = std::clamp(retryCount, 1, 5);
}

StunBindingResult StunClient::bindingRequest(const QString& host, quint16 port, quint16 localPort) const {
    StunBindingResult result{};
    if (host.trimmed().isEmpty() || port == 0) {
        result.error = QStringLiteral("invalid STUN endpoint");
        return result;
    }

    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4, localPort)) {
        result.error = socket.errorString();
        return result;
    }

    const QHostAddress stunHost(host.trimmed());
    if (stunHost.isNull()) {
        result.error = QStringLiteral("STUN host must be an IPv4 address");
        return result;
    }

    for (int attempt = 0; attempt < m_retryCount; ++attempt) {
        const QByteArray transactionId = makeTransactionId();
        const QByteArray request = buildBindingRequest(transactionId);
        const qint64 sent = socket.writeDatagram(request, stunHost, port);
        if (sent != request.size()) {
            result.error = socket.errorString();
            continue;
        }

        if (!socket.waitForReadyRead(m_timeoutMs)) {
            result.error = QStringLiteral("STUN binding timeout");
            continue;
        }

        while (socket.hasPendingDatagrams()) {
            const QByteArray datagram = socket.receiveDatagram().data();
            StunEndpoint mapped{};
            QString parseError;
            if (parseBindingResponse(datagram, transactionId, &mapped, &parseError)) {
                result.success = true;
                result.mappedAddress = mapped;
                result.error.clear();
                return result;
            }
            result.error = parseError;
        }
    }

    return result;
}

QByteArray StunClient::makeTransactionId() {
    QByteArray id;
    id.resize(12);
    for (int i = 0; i < id.size(); ++i) {
        id[i] = static_cast<char>(QRandomGenerator::global()->generate() & 0xFFU);
    }
    return id;
}

QByteArray StunClient::buildBindingRequest(const QByteArray& transactionId) {
    QByteArray packet;
    if (transactionId.size() != 12) {
        return packet;
    }
    appendU16(&packet, kStunBindingRequest);
    appendU16(&packet, 0);
    appendU32(&packet, kMagicCookie);
    packet.append(transactionId);
    return packet;
}

bool StunClient::parseBindingResponse(const QByteArray& datagram,
                                      const QByteArray& expectedTransactionId,
                                      StunEndpoint* mappedAddress,
                                      QString* error) {
    if (datagram.size() < kHeaderSize || expectedTransactionId.size() != 12) {
        if (error != nullptr) {
            *error = QStringLiteral("short STUN response");
        }
        return false;
    }
    if (readU16(datagram.constData()) != kStunBindingSuccess || readU32(datagram.constData() + 4) != kMagicCookie) {
        if (error != nullptr) {
            *error = QStringLiteral("not a STUN binding success response");
        }
        return false;
    }
    if (QByteArrayView(datagram.constData() + 8, 12) != QByteArrayView(expectedTransactionId)) {
        if (error != nullptr) {
            *error = QStringLiteral("STUN transaction id mismatch");
        }
        return false;
    }

    const int messageLength = readU16(datagram.constData() + 2);
    if (messageLength < 0 || datagram.size() < kHeaderSize + messageLength) {
        if (error != nullptr) {
            *error = QStringLiteral("truncated STUN attributes");
        }
        return false;
    }

    StunEndpoint fallback{};
    int offset = kHeaderSize;
    const int end = kHeaderSize + messageLength;
    while (offset + 4 <= end) {
        const quint16 attrType = readU16(datagram.constData() + offset);
        const quint16 attrLength = readU16(datagram.constData() + offset + 2);
        offset += 4;
        if (offset + attrLength > end) {
            break;
        }

        StunEndpoint parsed{};
        if ((attrType == kAttrXorMappedAddress || attrType == kAttrMappedAddress) &&
            parseAddressAttribute(attrType, datagram.constData() + offset, attrLength, &parsed)) {
            if (attrType == kAttrXorMappedAddress) {
                if (mappedAddress != nullptr) {
                    *mappedAddress = parsed;
                }
                return true;
            }
            fallback = parsed;
        }
        offset += paddedLength(attrLength);
    }

    if (fallback.isValid()) {
        if (mappedAddress != nullptr) {
            *mappedAddress = fallback;
        }
        return true;
    }

    if (error != nullptr) {
        *error = QStringLiteral("STUN response missing mapped address");
    }
    return false;
}

bool parseStunServerUrl(const QString& url, QString* host, quint16* port) {
    QString normalized = url.trimmed();
    if (normalized.startsWith(QStringLiteral("stun:"), Qt::CaseInsensitive)) {
        normalized = normalized.mid(5);
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
    if (host != nullptr) {
        *host = parsedHost.trimmed();
    }
    if (port != nullptr) {
        *port = parsedPort;
    }
    return true;
}

QString makeServerReflexiveCandidate(const QHostAddress& host, quint16 port, quint32 priority) {
    if (host.isNull() || port == 0) {
        return {};
    }
    return QStringLiteral("candidate:2 1 udp %1 %2 %3 typ srflx").arg(priority).arg(host.toString()).arg(port);
}

}  // namespace ice
