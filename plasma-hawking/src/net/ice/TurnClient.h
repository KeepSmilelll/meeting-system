#pragma once

#include "StunClient.h"

#include <QByteArray>
#include <QHostAddress>
#include <QString>

namespace ice {

struct TurnServerConfig {
    QString host{};
    quint16 port{3478};
    QString username{};
    QString credential{};
};

struct TurnAllocateResult {
    bool success{false};
    StunEndpoint relayedAddress{};
    StunEndpoint mappedAddress{};
    QString realm{};
    QString nonce{};
    QString error{};
};

class TurnClient {
public:
    static bool parseTurnServerUrl(const QString& url, TurnServerConfig* out);

    static QByteArray buildAllocateRequest(const QByteArray& transactionId);
    static QByteArray buildAuthenticatedAllocateRequest(const QByteArray& transactionId,
                                                        const QString& username,
                                                        const QString& realm,
                                                        const QString& nonce,
                                                        const QString& credential);
    static QByteArray buildCreatePermissionRequest(const QByteArray& transactionId,
                                                   const QHostAddress& peerAddress,
                                                   const QString& username,
                                                   const QString& realm,
                                                   const QString& nonce,
                                                   const QString& credential);
    static QByteArray buildSendIndication(const QHostAddress& peerAddress,
                                          quint16 peerPort,
                                          const QByteArray& payload);

    static bool parseAllocateResponse(const QByteArray& datagram,
                                      const QByteArray& expectedTransactionId,
                                      TurnAllocateResult* result);
    static bool parseCreatePermissionResponse(const QByteArray& datagram,
                                              const QByteArray& expectedTransactionId,
                                              QString* error = nullptr);
    static bool parseDataIndication(const QByteArray& datagram,
                                    StunEndpoint* peer,
                                    QByteArray* payload,
                                    QString* error = nullptr);

    static QString makeRelayCandidate(const QHostAddress& host, quint16 port, quint32 priority = 16777215U);
};

}  // namespace ice
