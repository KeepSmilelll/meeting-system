#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>

#include <cstdint>

namespace ice {

struct StunEndpoint {
    QHostAddress address{};
    quint16 port{0};

    bool isValid() const {
        return !address.isNull() && port != 0;
    }
};

struct StunBindingResult {
    bool success{false};
    StunEndpoint mappedAddress{};
    QString error{};
};

class StunClient {
public:
    StunClient();

    void setTimeoutMs(int timeoutMs);
    void setRetryCount(int retryCount);

    StunBindingResult bindingRequest(const QString& host, quint16 port, quint16 localPort = 0) const;

    static QByteArray makeTransactionId();
    static QByteArray buildBindingRequest(const QByteArray& transactionId);
    static bool parseBindingResponse(const QByteArray& datagram,
                                     const QByteArray& expectedTransactionId,
                                     StunEndpoint* mappedAddress,
                                     QString* error = nullptr);

private:
    int m_timeoutMs{250};
    int m_retryCount{2};
};

bool parseStunServerUrl(const QString& url, QString* host, quint16* port);
QString makeServerReflexiveCandidate(const QHostAddress& host, quint16 port, quint32 priority = 1694498815U);

}  // namespace ice
