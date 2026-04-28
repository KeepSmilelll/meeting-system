#include "SettingsRepository.h"

#include "DatabaseManager.h"

#include <QDateTime>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>

SettingsRepository::SettingsRepository(const QString& databasePath)
    : m_databasePath(databasePath.isEmpty() ? DatabaseManager::defaultDatabasePath() : databasePath)
    , m_connectionName(QStringLiteral("settings_repo_%1").arg(QString::number(reinterpret_cast<quintptr>(this), 16))) {
    open();
}

SettingsRepository::~SettingsRepository() {
    if (!m_db.isValid()) {
        return;
    }

    const QString name = m_connectionName;
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}

bool SettingsRepository::isOpen() const {
    return m_db.isValid() && m_db.isOpen();
}

bool SettingsRepository::saveValue(const QString& key, const QVariant& value, const QString& category) {
    if (!ensureOpen()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO settings(category, key, value, updated_at) VALUES(?, ?, ?, ?)"));
    query.addBindValue(category);
    query.addBindValue(key);
    query.addBindValue(value.toString());
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return query.exec();
}

QVariant SettingsRepository::value(const QString& key, const QVariant& defaultValue, const QString& category) const {
    if (!m_db.isValid() || !m_db.isOpen()) {
        return defaultValue;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE category = ? AND key = ? LIMIT 1"));
    query.addBindValue(category);
    query.addBindValue(key);
    if (!query.exec() || !query.next()) {
        return defaultValue;
    }

    return query.value(0).toString();
}

bool SettingsRepository::removeValue(const QString& key, const QString& category) {
    if (!ensureOpen()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM settings WHERE category = ? AND key = ?"));
    query.addBindValue(category);
    query.addBindValue(key);
    return query.exec();
}

bool SettingsRepository::contains(const QString& key, const QString& category) const {
    if (!m_db.isValid() || !m_db.isOpen()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT 1 FROM settings WHERE category = ? AND key = ? LIMIT 1"));
    query.addBindValue(category);
    query.addBindValue(key);
    return query.exec() && query.next();
}

bool SettingsRepository::saveToken(const QString& token) {
    return saveValue(QStringLiteral("token"), token, QStringLiteral("auth"));
}

QString SettingsRepository::token(const QString& defaultValue) const {
    return value(QStringLiteral("token"), defaultValue, QStringLiteral("auth")).toString();
}

bool SettingsRepository::clearToken() {
    return removeValue(QStringLiteral("token"), QStringLiteral("auth"));
}

bool SettingsRepository::saveSetting(const QString& key, const QString& value) {
    return saveValue(key, value, QString());
}

QString SettingsRepository::getSetting(const QString& key, const QString& defaultValue) const {
    return value(key, defaultValue, QString()).toString();
}

bool SettingsRepository::saveServerEndpoint(const QString& host, quint16 port) {
    return saveValue(QStringLiteral("server_host"), host, QStringLiteral("network")) &&
           saveValue(QStringLiteral("server_port"), QString::number(port), QStringLiteral("network"));
}

QString SettingsRepository::serverHost(const QString& defaultValue) const {
    return value(QStringLiteral("server_host"), defaultValue, QStringLiteral("network")).toString();
}

quint16 SettingsRepository::serverPort(quint16 defaultValue) const {
    const QString stored = value(QStringLiteral("server_port"), QString::number(defaultValue), QStringLiteral("network")).toString();
    bool ok = false;
    const int parsed = stored.toInt(&ok);
    if (!ok || parsed <= 0 || parsed > 65535) {
        return defaultValue;
    }
    return static_cast<quint16>(parsed);
}

bool SettingsRepository::saveIcePolicy(const QString& policy) {
    const QString normalized = policy.trimmed().toLower() == QStringLiteral("relay-only")
                                   ? QStringLiteral("relay-only")
                                   : QStringLiteral("all");
    return saveValue(QStringLiteral("ice_policy"), normalized, QStringLiteral("network"));
}

QString SettingsRepository::icePolicy(const QString& defaultValue) const {
    const QString normalized = value(QStringLiteral("ice_policy"), defaultValue, QStringLiteral("network")).toString().trimmed().toLower();
    return normalized == QStringLiteral("relay-only") ? QStringLiteral("relay-only") : QStringLiteral("all");
}

bool SettingsRepository::savePreferredCameraDevice(const QString& deviceName) {
    return saveValue(QStringLiteral("preferred_camera_device"), deviceName.trimmed(), QStringLiteral("media"));
}

QString SettingsRepository::preferredCameraDevice(const QString& defaultValue) const {
    return value(QStringLiteral("preferred_camera_device"), defaultValue, QStringLiteral("media")).toString().trimmed();
}

bool SettingsRepository::clearPreferredCameraDevice() {
    return removeValue(QStringLiteral("preferred_camera_device"), QStringLiteral("media"));
}
bool SettingsRepository::savePreferredMicrophoneDevice(const QString& deviceName) {
    return saveValue(QStringLiteral("preferred_microphone_device"), deviceName.trimmed(), QStringLiteral("media"));
}

QString SettingsRepository::preferredMicrophoneDevice(const QString& defaultValue) const {
    return value(QStringLiteral("preferred_microphone_device"), defaultValue, QStringLiteral("media")).toString().trimmed();
}

bool SettingsRepository::clearPreferredMicrophoneDevice() {
    return removeValue(QStringLiteral("preferred_microphone_device"), QStringLiteral("media"));
}

bool SettingsRepository::savePreferredSpeakerDevice(const QString& deviceName) {
    return saveValue(QStringLiteral("preferred_speaker_device"), deviceName.trimmed(), QStringLiteral("media"));
}

QString SettingsRepository::preferredSpeakerDevice(const QString& defaultValue) const {
    return value(QStringLiteral("preferred_speaker_device"), defaultValue, QStringLiteral("media")).toString().trimmed();
}

bool SettingsRepository::clearPreferredSpeakerDevice() {
    return removeValue(QStringLiteral("preferred_speaker_device"), QStringLiteral("media"));
}

bool SettingsRepository::clearSession() {
    return clearToken() &&
           removeValue(QStringLiteral("user_id"), QStringLiteral("auth")) &&
           removeValue(QStringLiteral("token_issued_at_ms"), QStringLiteral("auth")) &&
           removeValue(QStringLiteral("token_expires_at_ms"), QStringLiteral("auth"));
}

bool SettingsRepository::ensureOpen() {
    if (isOpen()) {
        return true;
    }
    return open();
}

bool SettingsRepository::open() {
    if (isOpen()) {
        return true;
    }

    m_db = DatabaseManager::instance().openDatabase(m_connectionName, m_databasePath);
    return isOpen();
}

bool SettingsRepository::initializeSchema() {
    return isOpen();
}

QString SettingsRepository::resolvedDatabasePath() const {
    return m_databasePath;
}

QString SettingsRepository::connectionName() const {
    return m_connectionName;
}

QString SettingsRepository::settingKey(const QString& category, const QString& key) {
    return category.isEmpty() ? key : category + QStringLiteral("/") + key;
}




