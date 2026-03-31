#pragma once

#include <QSqlDatabase>
#include <QString>

class MeetingRepository {
public:
    explicit MeetingRepository(const QString& databasePath = QString());
    ~MeetingRepository();

    bool isOpen() const;

    bool upsertMeeting(const QString& meetingId,
                       const QString& title,
                       const QString& hostUserId,
                       qint64 lastJoinedAt);
    bool markMeetingLeft(const QString& meetingId, qint64 lastLeftAt);

private:
    bool ensureOpen();
    bool open();
    bool initializeSchema();

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_db;
};
