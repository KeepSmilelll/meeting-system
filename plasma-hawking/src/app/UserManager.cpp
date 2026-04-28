#include "UserManager.h"

UserManager::UserManager(const QString& databasePath, QObject* parent)
    : QObject(parent)
    , m_settings(databasePath) {
    load();
}

bool UserManager::load() {
    const QString loadedUsername = m_settings.getSetting(QStringLiteral("username"));
    const QString loadedUserId = m_settings.value(QStringLiteral("user_id"), QString(), QStringLiteral("auth")).toString();
    (void)m_tokenManager.load(m_settings);
    const QString loadedToken = m_tokenManager.isExpired() ? QString() : m_tokenManager.token();
    const QString loadedHost = m_settings.serverHost(m_serverHost);
    const quint16 loadedPort = m_settings.serverPort(m_serverPort);
    const QString loadedIcePolicy = m_settings.icePolicy(m_icePolicy);
    const QString loadedPreferredCameraDevice = m_settings.preferredCameraDevice(m_preferredCameraDevice);
    const QString loadedPreferredMicrophoneDevice = m_settings.preferredMicrophoneDevice(m_preferredMicrophoneDevice);
    const QString loadedPreferredSpeakerDevice = m_settings.preferredSpeakerDevice(m_preferredSpeakerDevice);

    if (loadedToken.isEmpty() && !m_tokenManager.token().isEmpty()) {
        m_tokenManager.clear();
        (void)m_tokenManager.save(m_settings);
    }

    const bool didSettingsChange = (m_username != loadedUsername) ||
                                   (m_userId != loadedUserId) ||
                                   (m_token != loadedToken) ||
                                   (m_serverHost != loadedHost) ||
                                   (m_serverPort != loadedPort) ||
                                   (m_icePolicy != loadedIcePolicy) ||
                                   (m_preferredCameraDevice != loadedPreferredCameraDevice) ||
                                   (m_preferredMicrophoneDevice != loadedPreferredMicrophoneDevice) ||
                                   (m_preferredSpeakerDevice != loadedPreferredSpeakerDevice);

    m_username = loadedUsername;
    m_userId = loadedUserId;
    m_token = loadedToken;
    m_serverHost = loadedHost.isEmpty() ? QStringLiteral("127.0.0.1") : loadedHost;
    m_serverPort = loadedPort == 0 ? 8443 : loadedPort;
    m_icePolicy = loadedIcePolicy;
    m_preferredCameraDevice = loadedPreferredCameraDevice.trimmed();
    m_preferredMicrophoneDevice = loadedPreferredMicrophoneDevice.trimmed();
    m_preferredSpeakerDevice = loadedPreferredSpeakerDevice.trimmed();

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

QString UserManager::icePolicy() const {
    return m_icePolicy;
}

QString UserManager::preferredCameraDevice() const {
    return m_preferredCameraDevice;
}

QString UserManager::preferredMicrophoneDevice() const {
    return m_preferredMicrophoneDevice;
}

QString UserManager::preferredSpeakerDevice() const {
    return m_preferredSpeakerDevice;
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

void UserManager::setIcePolicy(const QString& policy) {
    const QString normalized = policy.trimmed().toLower() == QStringLiteral("relay-only")
                                   ? QStringLiteral("relay-only")
                                   : QStringLiteral("all");
    if (m_icePolicy == normalized) {
        return;
    }

    m_icePolicy = normalized;
    m_settings.saveIcePolicy(m_icePolicy);
    emit settingsChanged();
}

void UserManager::setPreferredCameraDevice(const QString& deviceName) {
    const QString normalized = deviceName.trimmed();
    if (m_preferredCameraDevice == normalized) {
        return;
    }

    m_preferredCameraDevice = normalized;
    if (m_preferredCameraDevice.isEmpty()) {
        m_settings.clearPreferredCameraDevice();
    } else {
        m_settings.savePreferredCameraDevice(m_preferredCameraDevice);
    }
    emit settingsChanged();
}

void UserManager::setPreferredMicrophoneDevice(const QString& deviceName) {
    const QString normalized = deviceName.trimmed();
    if (m_preferredMicrophoneDevice == normalized) {
        return;
    }

    m_preferredMicrophoneDevice = normalized;
    if (m_preferredMicrophoneDevice.isEmpty()) {
        m_settings.clearPreferredMicrophoneDevice();
    } else {
        m_settings.savePreferredMicrophoneDevice(m_preferredMicrophoneDevice);
    }
    emit settingsChanged();
}

void UserManager::setPreferredSpeakerDevice(const QString& deviceName) {
    const QString normalized = deviceName.trimmed();
    if (m_preferredSpeakerDevice == normalized) {
        return;
    }

    m_preferredSpeakerDevice = normalized;
    if (m_preferredSpeakerDevice.isEmpty()) {
        m_settings.clearPreferredSpeakerDevice();
    } else {
        m_settings.savePreferredSpeakerDevice(m_preferredSpeakerDevice);
    }
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
    m_tokenManager.setToken(m_token);
    (void)m_tokenManager.save(m_settings);

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
    m_tokenManager.clear();
    (void)m_settings.clearSession();
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
