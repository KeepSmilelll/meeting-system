#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QString>
#include <QVector>

#include "storage/MessageRepository.h"

class ChatMessageListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum ChatMessageRole {
        MessageIdRole = Qt::UserRole + 1,
        MeetingIdRole,
        SenderIdRole,
        SenderNameRole,
        ContentRole,
        MessageTypeRole,
        ReplyToIdRole,
        SentAtRole,
        LocalRole,
    };
    Q_ENUM(ChatMessageRole)

    explicit ChatMessageListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void clear();

    void replaceMessages(const QVector<MessageRepository::MessageRecord>& records);
    bool appendMessage(const MessageRepository::MessageRecord& record);

private:
    static QString stableMessageKey(const MessageRepository::MessageRecord& record);

    QVector<MessageRepository::MessageRecord> m_messages;
    QSet<QString> m_seenMessageKeys;
};
