#include <cassert>

#include <QCoreApplication>
#include <QDebug>
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

    const QByteArray masterKey(
        security::SRTPContext::masterKeyLength(security::SRTPContext::Profile::Aes128CmSha1_80),
        'K');
    const QByteArray masterSalt(
        security::SRTPContext::masterSaltLength(security::SRTPContext::Profile::Aes128CmSha1_80),
        'S');

    security::SRTPContext sender;
    security::SRTPContext receiver;
    assert(!sender.configured());
    assert(sender.configure(masterKey,
                            masterSalt,
                            security::SRTPContext::Direction::Outbound,
                            0x11223344U));
    assert(receiver.configure(masterKey,
                              masterSalt,
                              security::SRTPContext::Direction::Inbound,
                              0x11223344U));
    assert(sender.configured());
    assert(receiver.configured());
    assert(!sender.keyFingerprint().isEmpty());

    QByteArray rtpPacket = QByteArray::fromHex("806F0001000000641122334401020304");
    const QByteArray plainRtp = rtpPacket;
    assert(sender.protectRtp(&rtpPacket));
    assert(rtpPacket.size() > plainRtp.size());
    assert(receiver.unprotectRtp(&rtpPacket));
    assert(rtpPacket == plainRtp);

    QByteArray rtcpPacket = QByteArray::fromHex("80C80006112233440000000200000003000000040000000500000006");
    const QByteArray plainRtcp = rtcpPacket;
    const bool protectedRtcp = sender.protectRtcp(&rtcpPacket);
    if (!protectedRtcp) {
        qCritical().noquote() << "protectRtcp failed:" << sender.lastError();
    }
    assert(protectedRtcp);
    assert(rtcpPacket.size() > plainRtcp.size());
    assert(receiver.unprotectRtcp(&rtcpPacket));
    assert(rtcpPacket == plainRtcp);

    sender.clear();
    receiver.clear();
    assert(!sender.configured());
    assert(!receiver.configured());

    return 0;
}
