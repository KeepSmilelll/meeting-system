#pragma once

#include <QByteArray>
#include <QString>

namespace security {

class CryptoUtils {
public:
    static QByteArray sha256(const QByteArray& data);
    static QString sha256Hex(const QByteArray& data);

    static QByteArray hmacSha256(const QByteArray& key, const QByteArray& data);
    static QString hmacSha256Hex(const QByteArray& key, const QByteArray& data);

    static QString generateRandomToken(int numBytes = 32);
    static bool constantTimeEquals(const QByteArray& lhs, const QByteArray& rhs);
};

}  // namespace security
