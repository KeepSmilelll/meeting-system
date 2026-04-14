#include <cassert>

#include <QCoreApplication>
#include <QTemporaryDir>

#include "security/CryptoUtils.h"
#include "security/SRTPContext.h"
#include "security/TokenManager.h"
#include "storage/SettingsRepository.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    const QString sha = security::CryptoUtils::sha256Hex("abc");
    assert(sha == QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    const QByteArray hmac = security::CryptoUtils::hmacSha256("key", "value");
    assert(!hmac.isEmpty());
    assert(security::CryptoUtils::constantTimeEquals(hmac, hmac));

    const QString randomToken = security::CryptoUtils::generateRandomToken(24);
    assert(!randomToken.isEmpty());

    security::TokenManager tokenManager;
    assert(tokenManager.setToken(QStringLiteral("token-A"), 1000, 100));
    assert(tokenManager.hasToken());
    assert(!tokenManager.isExpired(1099));
    assert(tokenManager.isExpired(1200));
    assert(!tokenManager.tokenFingerprint().isEmpty());

    QTemporaryDir tempDir;
    assert(tempDir.isValid());

    const QString dbPath = tempDir.filePath(QStringLiteral("security_test.sqlite"));
    SettingsRepository settings(dbPath);
    assert(settings.isOpen());

    assert(tokenManager.save(settings));

    security::TokenManager loaded;
    assert(loaded.load(settings));
    assert(loaded.token() == QStringLiteral("token-A"));
    assert(loaded.issuedAtMs() == 100);
    assert(loaded.expiresAtMs() == 1100);

    security::SRTPContext srtp;
    assert(!srtp.configured());
    assert(srtp.configure("master-key", "master-salt"));
    assert(srtp.configured());
    assert(!srtp.keyFingerprint().isEmpty());
    srtp.clear();
    assert(!srtp.configured());

    return 0;
}
