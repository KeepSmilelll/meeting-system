#include "UserRepository.h"

#include "DatabaseManager.h"

#include <QDateTime>
#include <QSqlQuery>
#include <QtGlobal>

namespace {

QString makeConnectionName(const char* prefix, const void* object) {
    return QStringLiteral("%1_%2")
        .arg(QString::fromLatin1(prefix))
        .arg(QString::number(reinterpret_cast<quintptr>(object), 16));
}

}  // namespace

UserRepository::UserRepository(const QString& databasePath)
    : m_databasePath(databasePath.isEmpty() ? DatabaseManager::defaultDatabasePath() : databasePath)
    , m_connectionName(makeConnectionName("user_repo", this)) {
    open();
}

UserRepository::~UserRepository() {
    if (!m_db.isValid()) {
        return;
    }

    const QString name = m_connectionName;
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}

bool UserRepository::isOpen() const {
    return m_db.isValid() && m_db.isOpen();
}

bool UserRepository::upsertUser(const QString& userId,
                                const QString& username,
                                const QString& displayName,
                                const QString& avatarUrl,
                                int status,
                                const QString& token,
                                qint64 lastSeenAt) {
    if (!ensureOpen() || userId.trimmed().isEmpty()) {
        return false;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO user(user_id, username, display_name, avatar_url, status, token, last_seen_at, created_at, updated_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(user_id) DO UPDATE SET "
        "username=excluded.username, "
        "display_name=excluded.display_name, "
        "avatar_url=excluded.avatar_url, "
        "status=excluded.status, "
        "token=excluded.token, "
        "last_seen_at=excluded.last_seen_at, "
        "updated_at=excluded.updated_at"));
    query.addBindValue(userId.trimmed());
    query.addBindValue(username.trimmed());
    query.addBindValue(displayName.trimmed());
    query.addBindValue(avatarUrl.trimmed());
    query.addBindValue(status);
    query.addBindValue(token);
    query.addBindValue(lastSeenAt > 0 ? lastSeenAt : now);
    query.addBindValue(now);
    query.addBindValue(now);
    return query.exec();
}

QVariantMap UserRepository::findById(const QString& userId) const {
    QVariantMap result;
    if (!m_db.isValid() || !m_db.isOpen() || userId.trimmed().isEmpty()) {
        return result;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT user_id, username, display_name, avatar_url, status, token, last_seen_at, created_at, updated_at "
        "FROM user WHERE user_id = ? LIMIT 1"));
    query.addBindValue(userId.trimmed());
    if (!query.exec() || !query.next()) {
        return result;
    }

    result.insert(QStringLiteral("user_id"), query.value(0).toString());
    result.insert(QStringLiteral("username"), query.value(1).toString());
    result.insert(QStringLiteral("display_name"), query.value(2).toString());
    result.insert(QStringLiteral("avatar_url"), query.value(3).toString());
    result.insert(QStringLiteral("status"), query.value(4).toInt());
    result.insert(QStringLiteral("token"), query.value(5).toString());
    result.insert(QStringLiteral("last_seen_at"), query.value(6).toLongLong());
    result.insert(QStringLiteral("created_at"), query.value(7).toLongLong());
    result.insert(QStringLiteral("updated_at"), query.value(8).toLongLong());
    return result;
}

bool UserRepository::setStatus(const QString& userId, int status) {
    if (!ensureOpen() || userId.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE user SET status = ?, updated_at = ? WHERE user_id = ?"));
    query.addBindValue(status);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(userId.trimmed());
    return query.exec();
}

bool UserRepository::saveToken(const QString& userId, const QString& token) {
    if (!ensureOpen() || userId.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE user SET token = ?, updated_at = ? WHERE user_id = ?"));
    query.addBindValue(token);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(userId.trimmed());
    return query.exec();
}

bool UserRepository::clearToken(const QString& userId) {
    return saveToken(userId, QString());
}

bool UserRepository::ensureOpen() {
    if (isOpen()) {
        return true;
    }
    return open();
}

bool UserRepository::open() {
    if (isOpen()) {
        return true;
    }

    m_db = DatabaseManager::instance().openDatabase(m_connectionName, m_databasePath);
    return isOpen();
}

