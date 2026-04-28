#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVariant>

class SettingsRepository {
public:
    explicit SettingsRepository(const QString& databasePath = QString());
    ~SettingsRepository();

    bool isOpen() const;

    bool saveValue(const QString& key, const QVariant& value, const QString& category = QString());
    QVariant value(const QString& key,
                   const QVariant& defaultValue = QVariant(),
                   const QString& category = QString()) const;
    bool removeValue(const QString& key, const QString& category = QString());
    bool contains(const QString& key, const QString& category = QString()) const;

    bool saveToken(const QString& token);
    QString token(const QString& defaultValue = QString()) const;
    bool clearToken();

    bool saveSetting(const QString& key, const QString& value);
    QString getSetting(const QString& key, const QString& defaultValue = QString()) const;

    bool saveServerEndpoint(const QString& host, quint16 port);
    QString serverHost(const QString& defaultValue = QStringLiteral("127.0.0.1")) const;
    quint16 serverPort(quint16 defaultValue = 8443) const;
    bool saveIcePolicy(const QString& policy);
    QString icePolicy(const QString& defaultValue = QStringLiteral("all")) const;
    bool savePreferredCameraDevice(const QString& deviceName);
    QString preferredCameraDevice(const QString& defaultValue = QString()) const;
    bool clearPreferredCameraDevice();
    bool savePreferredMicrophoneDevice(const QString& deviceName);
    QString preferredMicrophoneDevice(const QString& defaultValue = QString()) const;
    bool clearPreferredMicrophoneDevice();
    bool savePreferredSpeakerDevice(const QString& deviceName);
    QString preferredSpeakerDevice(const QString& defaultValue = QString()) const;
    bool clearPreferredSpeakerDevice();

    bool clearSession();

private:
    bool ensureOpen();
    bool open();
    bool initializeSchema();
    QString resolvedDatabasePath() const;
    QString connectionName() const;

    static QString settingKey(const QString& category, const QString& key);

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_db;
};
