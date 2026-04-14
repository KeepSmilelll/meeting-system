#include "TokenManager.h"

#include "CryptoUtils.h"
#include "storage/SettingsRepository.h"

#include <QDateTime>

namespace security {

namespace {

qint64 effectiveNowMs(qint64 nowMs) {
    return nowMs > 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
}

}  // namespace

bool TokenManager::setToken(const QString& token, qint64 ttlMs, qint64 nowMs) {
    const QString normalized = token.trimmed();
    if (normalized.isEmpty()) {
        clear();
        return false;
    }

    const qint64 now = effectiveNowMs(nowMs);
    m_token = normalized;
    m_issuedAtMs = now;
    m_expiresAtMs = ttlMs > 0 ? (now + ttlMs) : 0;
    return true;
}

void TokenManager::clear() {
    m_token.clear();
    m_issuedAtMs = 0;
    m_expiresAtMs = 0;
}

QString TokenManager::token() const {
    return m_token;
}

bool TokenManager::hasToken() const {
    return !m_token.isEmpty();
}

bool TokenManager::isExpired(qint64 nowMs) const {
    if (m_token.isEmpty()) {
        return true;
    }
    if (m_expiresAtMs <= 0) {
        return false;
    }

    return effectiveNowMs(nowMs) > m_expiresAtMs;
}

qint64 TokenManager::issuedAtMs() const {
    return m_issuedAtMs;
}

qint64 TokenManager::expiresAtMs() const {
    return m_expiresAtMs;
}

QString TokenManager::tokenFingerprint() const {
    if (m_token.isEmpty()) {
        return QString();
    }
    return CryptoUtils::sha256Hex(m_token.toUtf8()).left(16);
}

bool TokenManager::load(const SettingsRepository& settings) {
    const QString loadedToken = settings.token();
    const qint64 loadedIssuedAt = settings.value(QStringLiteral("token_issued_at_ms"), 0, QStringLiteral("auth")).toLongLong();
    const qint64 loadedExpiresAt = settings.value(QStringLiteral("token_expires_at_ms"), 0, QStringLiteral("auth")).toLongLong();

    if (loadedToken.trimmed().isEmpty()) {
        clear();
        return false;
    }

    m_token = loadedToken.trimmed();
    m_issuedAtMs = loadedIssuedAt;
    m_expiresAtMs = loadedExpiresAt;
    return true;
}

bool TokenManager::save(SettingsRepository& settings) const {
    const bool tokenSaved = settings.saveToken(m_token);
    const bool issuedSaved = settings.saveValue(QStringLiteral("token_issued_at_ms"),
                                                QString::number(m_issuedAtMs),
                                                QStringLiteral("auth"));
    const bool expiresSaved = settings.saveValue(QStringLiteral("token_expires_at_ms"),
                                                 QString::number(m_expiresAtMs),
                                                 QStringLiteral("auth"));
    return tokenSaved && issuedSaved && expiresSaved;
}

}  // namespace security
