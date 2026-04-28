#include "ChatMessageListModel.h"

ChatMessageListModel::ChatMessageListModel(QObject* parent)
    : QAbstractListModel(parent) {}

int ChatMessageListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_messages.size();
}

QVariant ChatMessageListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_messages.size()) {
        return {};
    }

    const auto& item = m_messages.at(index.row());
    switch (role) {
    case MessageIdRole:
        return item.remoteMessageId.isEmpty() ? QString::number(item.id) : item.remoteMessageId;
    case MeetingIdRole:
        return item.meetingId;
    case SenderIdRole:
        return item.senderId;
    case SenderNameRole:
        return item.senderName;
    case ContentRole:
        return item.content;
    case MessageTypeRole:
        return item.messageType;
    case ReplyToIdRole:
        return item.replyToId;
    case SentAtRole:
        return item.sentAt;
    case LocalRole:
        return item.isLocal;
    default:
        return {};
    }
}

QHash<int, QByteArray> ChatMessageListModel::roleNames() const {
    static const QHash<int, QByteArray> roles{
        {MessageIdRole, "messageId"},
        {MeetingIdRole, "meetingId"},
        {SenderIdRole, "senderId"},
        {SenderNameRole, "senderName"},
        {ContentRole, "content"},
        {MessageTypeRole, "messageType"},
        {ReplyToIdRole, "replyToId"},
        {SentAtRole, "sentAt"},
        {LocalRole, "local"},
    };
    return roles;
}

void ChatMessageListModel::clear() {
    if (m_messages.isEmpty() && m_seenMessageKeys.isEmpty()) {
        return;
    }

    beginResetModel();
    m_messages.clear();
    m_seenMessageKeys.clear();
    endResetModel();
}

void ChatMessageListModel::replaceMessages(const QVector<MessageRepository::MessageRecord>& records) {
    beginResetModel();
    m_messages.clear();
    m_seenMessageKeys.clear();
    m_messages.reserve(records.size());
    for (const auto& record : records) {
        const QString key = stableMessageKey(record);
        if (!key.isEmpty() && m_seenMessageKeys.contains(key)) {
            continue;
        }
        m_messages.append(record);
        if (!key.isEmpty()) {
            m_seenMessageKeys.insert(key);
        }
    }
    endResetModel();
}

bool ChatMessageListModel::appendMessage(const MessageRepository::MessageRecord& record) {
    const QString key = stableMessageKey(record);
    if (!key.isEmpty() && m_seenMessageKeys.contains(key)) {
        return false;
    }

    const int row = m_messages.size();
    beginInsertRows(QModelIndex(), row, row);
    m_messages.append(record);
    if (!key.isEmpty()) {
        m_seenMessageKeys.insert(key);
    }
    endInsertRows();
    return true;
}

QString ChatMessageListModel::stableMessageKey(const MessageRepository::MessageRecord& record) {
    if (!record.remoteMessageId.trimmed().isEmpty()) {
        return record.meetingId + QLatin1Char('|') + record.remoteMessageId.trimmed();
    }
    if (record.id > 0) {
        return record.meetingId + QLatin1Char('|') + QString::number(record.id);
    }
    return {};
}
