#pragma once

#include <QSqlDatabase>
#include <QString>

class CallLogRepository {
public:
    explicit CallLogRepository(const QString& databasePath = QString());
    ~CallLogRepository();

    bool isOpen() const;

    bool startCall(const QString& meetingId,
                   const QString& title,
                   const QString& userId,
                   qint64 joinedAt,
                   bool wasHost);
    bool finishActiveCall(const QString& meetingId,
                          const QString& userId,
                          const QString& leaveReason,
                          qint64 leftAt);

private:
    bool ensureOpen();
    bool open();
    bool initializeSchema();

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_db;
};
