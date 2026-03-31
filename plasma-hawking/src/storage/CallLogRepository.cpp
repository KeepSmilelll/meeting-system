#include "CallLogRepository.h"

#include "DatabaseManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>

CallLogRepository::CallLogRepository(const QString& databasePath)
    : m_databasePath(databasePath.isEmpty() ? DatabaseManager::defaultDatabasePath() : databasePath)
    , m_connectionName(QStringLiteral("call_log_repo_%1").arg(QString::number(reinterpret_cast<quintptr>(this), 16))) {
    open();
}

CallLogRepository::~CallLogRepository() {
    if (!m_db.isValid()) {
        return;
    }

    const QString name = m_connectionName;
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}

bool CallLogRepository::isOpen() const {
    return m_db.isValid() && m_db.isOpen();
}

bool CallLogRepository::startCall(const QString& meetingId,
                                  const QString& title,
                                  const QString& userId,
                                  qint64 joinedAt,
                                  bool wasHost) {
    if (!ensureOpen() || meetingId.trimmed().isEmpty() || userId.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO call_log(meeting_id, title, user_id, joined_at, left_at, leave_reason, was_host) "
        "VALUES(?, ?, ?, ?, NULL, '', ?)"));
    query.addBindValue(meetingId.trimmed());
    query.addBindValue(title.trimmed());
    query.addBindValue(userId.trimmed());
    query.addBindValue(joinedAt);
    query.addBindValue(wasHost ? 1 : 0);
    return query.exec();
}

bool CallLogRepository::finishActiveCall(const QString& meetingId,
                                         const QString& userId,
                                         const QString& leaveReason,
                                         qint64 leftAt) {
    if (!ensureOpen() || meetingId.trimmed().isEmpty() || userId.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE call_log SET left_at = ?, leave_reason = ? "
        "WHERE id = ("
        "  SELECT id FROM call_log "
        "  WHERE meeting_id = ? AND user_id = ? AND left_at IS NULL "
        "  ORDER BY joined_at DESC, id DESC LIMIT 1"
        ")"));
    query.addBindValue(leftAt);
    query.addBindValue(leaveReason.trimmed());
    query.addBindValue(meetingId.trimmed());
    query.addBindValue(userId.trimmed());
    return query.exec();
}

bool CallLogRepository::ensureOpen() {
    if (isOpen()) {
        return true;
    }
    return open();
}

bool CallLogRepository::open() {
    if (isOpen()) {
        return true;
    }

    m_db = DatabaseManager::instance().openDatabase(m_connectionName, m_databasePath);
    return isOpen();
}

bool CallLogRepository::initializeSchema() {
    return isOpen();
}
