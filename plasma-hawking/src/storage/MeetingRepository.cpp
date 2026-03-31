#include "MeetingRepository.h"

#include "DatabaseManager.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>

MeetingRepository::MeetingRepository(const QString& databasePath)
    : m_databasePath(databasePath.isEmpty() ? DatabaseManager::defaultDatabasePath() : databasePath)
    , m_connectionName(QStringLiteral("meeting_repo_%1").arg(QString::number(reinterpret_cast<quintptr>(this), 16))) {
    open();
}

MeetingRepository::~MeetingRepository() {
    if (!m_db.isValid()) {
        return;
    }

    const QString name = m_connectionName;
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}

bool MeetingRepository::isOpen() const {
    return m_db.isValid() && m_db.isOpen();
}

bool MeetingRepository::upsertMeeting(const QString& meetingId,
                                      const QString& title,
                                      const QString& hostUserId,
                                      qint64 lastJoinedAt) {
    if (!ensureOpen() || meetingId.trimmed().isEmpty()) {
        return false;
    }

    const qint64 updatedAt = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO meeting(meeting_id, title, host_user_id, last_joined_at, updated_at) "
        "VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(meeting_id) DO UPDATE SET "
        "title=excluded.title, "
        "host_user_id=excluded.host_user_id, "
        "last_joined_at=excluded.last_joined_at, "
        "updated_at=excluded.updated_at"));
    query.addBindValue(meetingId.trimmed());
    query.addBindValue(title.trimmed());
    query.addBindValue(hostUserId.trimmed());
    query.addBindValue(lastJoinedAt);
    query.addBindValue(updatedAt);
    return query.exec();
}

bool MeetingRepository::markMeetingLeft(const QString& meetingId, qint64 lastLeftAt) {
    if (!ensureOpen() || meetingId.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE meeting SET last_left_at = ?, updated_at = ? WHERE meeting_id = ?"));
    query.addBindValue(lastLeftAt);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(meetingId.trimmed());
    return query.exec();
}

bool MeetingRepository::ensureOpen() {
    if (isOpen()) {
        return true;
    }
    return open();
}

bool MeetingRepository::open() {
    if (isOpen()) {
        return true;
    }

    m_db = DatabaseManager::instance().openDatabase(m_connectionName, m_databasePath);
    return isOpen();
}

bool MeetingRepository::initializeSchema() {
    return isOpen();
}
