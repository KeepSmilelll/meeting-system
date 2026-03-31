#include "UserManager.h"

UserManager::UserManager(const QString& databasePath, QObject* parent)
    : QObject(parent)
    , m_settings(databasePath) {
    load();
}

bool UserManager::load() {
    const QString loadedUsername = m_settings.getSetting(QStringLiteral("username"));
    const QString loadedUserId = m_settings.value(QStringLiteral("user_id"), QString(), QStringLiteral("auth")).toString();
    const QString loadedToken = m_settings.token();
    const QString loadedHost = m_settings.serverHost(m_serverHost);
    const quint16 loadedPort = m_settings.serverPort(m_serverPort);

    const bool didSettingsChange = (m_username != loadedUsername) ||
                                   (m_userId != loadedUserId) ||
                                   (m_token != loadedToken) ||
                                   (m_serverHost != loadedHost) ||
                                   (m_serverPort != loadedPort);

    m_username = loadedUsername;
    m_userId = loadedUserId;
    m_token = loadedToken;
    m_serverHost = loadedHost.isEmpty() ? QStringLiteral("127.0.0.1") : loadedHost;
    m_serverPort = loadedPort == 0 ? 8443 : loadedPort;

    const bool previousLoggedIn = m_loggedIn;
    syncLoggedInState();

    if (didSettingsChange) {
        emit sessionChanged();
        emit settingsChanged();
    }
    if (previousLoggedIn != m_loggedIn) {
        emit loggedInChanged();
    }

    return true;
}

bool UserManager::loggedIn() const {
    return m_loggedIn;
}

bool UserManager::hasCachedSession() const {
    return !m_username.isEmpty() && !m_token.isEmpty();
}

QString UserManager::username() const {
    return m_username;
}

QString UserManager::userId() const {
    return m_userId;
}

QString UserManager::token() const {
    return m_token;
}

QString UserManager::serverHost() const {
    return m_serverHost;
}

quint16 UserManager::serverPort() const {
    return m_serverPort;
}

void UserManager::setServerHost(const QString& host) {
    const QString normalized = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    if (m_serverHost == normalized) {
        return;
    }

    m_serverHost = normalized;
    m_settings.saveServerEndpoint(m_serverHost, m_serverPort);
    emit settingsChanged();
}

void UserManager::setServerPort(quint16 port) {
    if (port == 0 || m_serverPort == port) {
        return;
    }

    m_serverPort = port;
    m_settings.saveServerEndpoint(m_serverHost, m_serverPort);
    emit settingsChanged();
}

void UserManager::setServerEndpoint(const QString& host, quint16 port) {
    const QString normalized = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    if (m_serverHost == normalized && m_serverPort == port) {
        return;
    }

    m_serverHost = normalized;
    m_serverPort = port == 0 ? 8443 : port;
    m_settings.saveServerEndpoint(m_serverHost, m_serverPort);
    emit settingsChanged();
}

void UserManager::setSession(const QString& username, const QString& userId, const QString& token) {
    const QString normalizedUsername = username.trimmed();
    if (m_username == normalizedUsername && m_userId == userId && m_token == token) {
        syncLoggedInState();
        return;
    }

    m_username = normalizedUsername;
    m_userId = userId;
    m_token = token;
    m_settings.saveSetting(QStringLiteral("username"), m_username);
    m_settings.saveValue(QStringLiteral("user_id"), m_userId, QStringLiteral("auth"));
    m_settings.saveToken(m_token);
    syncLoggedInState();
    emit sessionChanged();
    emit loggedInChanged();
}

void UserManager::clearToken() {
    if (m_token.isEmpty()) {
        return;
    }

    m_token.clear();
    m_userId.clear();
    m_settings.clearSession();
    syncLoggedInState();
    emit sessionChanged();
    emit loggedInChanged();
}

void UserManager::clearSession() {
    clearToken();
}

void UserManager::syncLoggedInState() {
    m_loggedIn = !m_token.isEmpty();
}
