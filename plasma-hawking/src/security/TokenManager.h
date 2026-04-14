#pragma once

#include <QString>
#include <QtGlobal>

class SettingsRepository;

namespace security {

class TokenManager {
public:
    TokenManager() = default;

    bool setToken(const QString& token, qint64 ttlMs = 0, qint64 nowMs = 0);
    void clear();

    QString token() const;
    bool hasToken() const;
    bool isExpired(qint64 nowMs = 0) const;
    qint64 issuedAtMs() const;
    qint64 expiresAtMs() const;

    QString tokenFingerprint() const;

    bool load(const SettingsRepository& settings);
    bool save(SettingsRepository& settings) const;

private:
    QString m_token;
    qint64 m_issuedAtMs{0};
    qint64 m_expiresAtMs{0};
};

}  // namespace security
