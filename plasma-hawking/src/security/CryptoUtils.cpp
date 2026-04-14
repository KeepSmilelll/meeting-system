#include "CryptoUtils.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

namespace security {

QByteArray CryptoUtils::sha256(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString CryptoUtils::sha256Hex(const QByteArray& data) {
    return QString::fromLatin1(sha256(data).toHex());
}

QByteArray CryptoUtils::hmacSha256(const QByteArray& key, const QByteArray& data) {
    // RFC 2104 HMAC over SHA-256.
    constexpr int kBlockSize = 64;
    QByteArray normalizedKey = key;
    if (normalizedKey.size() > kBlockSize) {
        normalizedKey = sha256(normalizedKey);
    }
    if (normalizedKey.size() < kBlockSize) {
        normalizedKey.append(QByteArray(kBlockSize - normalizedKey.size(), '\0'));
    }

    QByteArray oKeyPad(kBlockSize, static_cast<char>(0x5C));
    QByteArray iKeyPad(kBlockSize, static_cast<char>(0x36));
    for (int index = 0; index < kBlockSize; ++index) {
        oKeyPad[index] = static_cast<char>(oKeyPad[index] ^ normalizedKey[index]);
        iKeyPad[index] = static_cast<char>(iKeyPad[index] ^ normalizedKey[index]);
    }

    return sha256(oKeyPad + sha256(iKeyPad + data));
}

QString CryptoUtils::hmacSha256Hex(const QByteArray& key, const QByteArray& data) {
    return QString::fromLatin1(hmacSha256(key, data).toHex());
}

QString CryptoUtils::generateRandomToken(int numBytes) {
    if (numBytes <= 0) {
        numBytes = 32;
    }

    QByteArray bytes(numBytes, '\0');
    for (int i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);
    }

    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool CryptoUtils::constantTimeEquals(const QByteArray& lhs, const QByteArray& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    quint8 diff = 0;
    for (int index = 0; index < lhs.size(); ++index) {
        diff |= static_cast<quint8>(lhs[index] ^ rhs[index]);
    }
    return diff == 0;
}

}  // namespace security
