#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

#include "security/TokenManager.h"
#include "storage/SettingsRepository.h"

class UserManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(QString username READ username NOTIFY sessionChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY sessionChanged)
    Q_PROPERTY(QString token READ token NOTIFY sessionChanged)
    Q_PROPERTY(QString serverHost READ serverHost WRITE setServerHost NOTIFY settingsChanged)
    Q_PROPERTY(quint16 serverPort READ serverPort WRITE setServerPort NOTIFY settingsChanged)
    Q_PROPERTY(QString icePolicy READ icePolicy WRITE setIcePolicy NOTIFY settingsChanged)
    Q_PROPERTY(QString preferredCameraDevice READ preferredCameraDevice WRITE setPreferredCameraDevice NOTIFY settingsChanged)
    Q_PROPERTY(QString preferredMicrophoneDevice READ preferredMicrophoneDevice WRITE setPreferredMicrophoneDevice NOTIFY settingsChanged)
    Q_PROPERTY(QString preferredSpeakerDevice READ preferredSpeakerDevice WRITE setPreferredSpeakerDevice NOTIFY settingsChanged)

public:
    explicit UserManager(const QString& databasePath = QString(), QObject* parent = nullptr);

    bool load();

    bool loggedIn() const;
    bool hasCachedSession() const;
    QString username() const;
    QString userId() const;
    QString token() const;
    QString serverHost() const;
    quint16 serverPort() const;
    QString icePolicy() const;
    QString preferredCameraDevice() const;
    QString preferredMicrophoneDevice() const;
    QString preferredSpeakerDevice() const;

public slots:
    void setServerHost(const QString& host);
    void setServerPort(quint16 port);
    void setServerEndpoint(const QString& host, quint16 port);
    void setIcePolicy(const QString& policy);
    void setPreferredCameraDevice(const QString& deviceName);
    void setPreferredMicrophoneDevice(const QString& deviceName);
    void setPreferredSpeakerDevice(const QString& deviceName);
    void setSession(const QString& username, const QString& userId, const QString& token);
    void clearToken();
    void clearSession();

signals:
    void loggedInChanged();
    void sessionChanged();
    void settingsChanged();

private:
    void syncLoggedInState();

    SettingsRepository m_settings;
    security::TokenManager m_tokenManager;
    QString m_username;
    QString m_userId;
    QString m_token;
    QString m_serverHost{QStringLiteral("127.0.0.1")};
    quint16 m_serverPort{8443};
    QString m_icePolicy{QStringLiteral("all")};
    QString m_preferredCameraDevice;
    QString m_preferredMicrophoneDevice;
    QString m_preferredSpeakerDevice;
    bool m_loggedIn{false};
};
