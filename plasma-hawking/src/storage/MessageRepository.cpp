#include "MessageRepository.h"

#include "DatabaseManager.h"

#include <QDateTime>
#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QtGlobal>

namespace {

QString makeConnectionName(const char* prefix, const void* object) {
    return QStringLiteral("%1_%2")
        .arg(QString::fromLatin1(prefix))
        .arg(QString::number(reinterpret_cast<quintptr>(object), 16));
}

MessageRepository::MessageRecord recordFromQuery(QSqlQuery& query) {
    MessageRepository::MessageRecord record;
    record.id = query.value(0).toLongLong();
    record.meetingId = query.value(1).toString();
    record.senderId = query.value(2).toString();
    record.senderName = query.value(3).toString();
    record.content = query.value(4).toString();
    record.sentAt = query.value(5).toLongLong();
    return record;
}

}  // namespace

MessageRepository::MessageRepository(const QString& databasePath)
    : m_databasePath(databasePath.isEmpty() ? DatabaseManager::defaultDatabasePath() : databasePath)
    , m_connectionName(makeConnectionName("message_repo", this)) {
    open();
}

MessageRepository::~MessageRepository() {
    if (!m_db.isValid()) {
        return;
    }

    const QString name = m_connectionName;
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}

bool MessageRepository::isOpen() const {
    return m_db.isValid() && m_db.isOpen();
}

bool MessageRepository::saveMessage(const QString& meetingId,
                                    const QString& senderId,
                                    const QString& senderName,
                                    const QString& content,
                                    qint64 sentAt,
                                    int messageType,
                                    const QString& replyToId,
                                    bool isLocal) {
    if (!ensureOpen() || meetingId.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO message(meeting_id, sender_id, sender_name, content, message_type, reply_to_id, sent_at, is_local, created_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(meetingId.trimmed());
    query.addBindValue(senderId.trimmed());
    query.addBindValue(senderName.trimmed());
    query.addBindValue(content);
    query.addBindValue(messageType);
    const QString normalizedReplyToId = replyToId.trimmed().isNull() ? QStringLiteral("") : replyToId.trimmed();
    query.addBindValue(normalizedReplyToId);
    query.addBindValue(sentAt > 0 ? sentAt : QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(isLocal ? 1 : 0);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        const QByteArray errorText = query.lastError().text().toLocal8Bit();
        std::fprintf(stderr, "MessageRepository::saveMessage failed: %s\n", errorText.constData());
        std::fflush(stderr);
        return false;
    }
    return true;
}

QVector<MessageRepository::MessageRecord> MessageRepository::listByMeeting(const QString& meetingId, int limit) const {
    QVector<MessageRecord> records;
    if (!m_db.isValid() || !m_db.isOpen() || meetingId.trimmed().isEmpty()) {
        return records;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT id, meeting_id, sender_id, sender_name, content, sent_at "
        "FROM message WHERE meeting_id = ? ORDER BY sent_at DESC, id DESC LIMIT ?"));
    query.addBindValue(meetingId.trimmed());
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        return records;
    }

    while (query.next()) {
        records.append(recordFromQuery(query));
    }
    return records;
}

QVector<MessageRepository::MessageRecord> MessageRepository::searchMessages(const QString& keyword, int limit) const {
    QVector<MessageRecord> records;
    const QString trimmed = keyword.trimmed();
    if (!m_db.isValid() || !m_db.isOpen() || trimmed.isEmpty()) {
        return records;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT m.id, m.meeting_id, m.sender_id, m.sender_name, m.content, m.sent_at "
        "FROM message_fts f JOIN message m ON m.id = f.rowid "
        "WHERE message_fts MATCH ? ORDER BY m.sent_at DESC, m.id DESC LIMIT ?"));
    query.addBindValue(trimmed);
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        return records;
    }

    while (query.next()) {
        records.append(recordFromQuery(query));
    }
    return records;
}

bool MessageRepository::ensureOpen() {
    if (isOpen()) {
        return true;
    }
    return open();
}

bool MessageRepository::open() {
    if (isOpen()) {
        return true;
    }

    m_db = DatabaseManager::instance().openDatabase(m_connectionName, m_databasePath);
    return isOpen();
}





