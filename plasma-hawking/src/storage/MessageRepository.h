#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

class MessageRepository {
public:
    struct MessageRecord {
        qint64 id{0};
        QString meetingId;
        QString senderId;
        QString senderName;
        QString content;
        qint64 sentAt{0};
    };

    explicit MessageRepository(const QString& databasePath = QString());
    ~MessageRepository();

    bool isOpen() const;

    bool saveMessage(const QString& meetingId,
                     const QString& senderId,
                     const QString& senderName,
                     const QString& content,
                     qint64 sentAt,
                     int messageType = 0,
                     const QString& replyToId = QString(),
                     bool isLocal = false);
    QVector<MessageRecord> listByMeeting(const QString& meetingId, int limit = 50) const;
    QVector<MessageRecord> searchMessages(const QString& keyword, int limit = 20) const;

private:
    bool ensureOpen();
    bool open();

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_db;
};
