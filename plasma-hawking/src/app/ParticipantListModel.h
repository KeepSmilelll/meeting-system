#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QVector>

namespace meeting {
class Participant;
}

class ParticipantListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum ParticipantRole {
        UserIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        AvatarUrlRole,
        RoleRole,
        AudioOnRole,
        VideoOnRole,
        SharingRole,
        HostRole,
    };
    Q_ENUM(ParticipantRole)

    struct ParticipantItem {
        QString userId;
        QString displayName;
        QString avatarUrl;
        int role{0};
        bool audioOn{true};
        bool videoOn{true};
        bool sharing{false};
        bool host{false};
    };

    explicit ParticipantListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void clearParticipants();
    Q_INVOKABLE void replaceParticipantsFromDisplayList(const QStringList& participants);
    void replaceParticipants(const QVector<ParticipantItem>& participants);
    Q_INVOKABLE void upsertParticipant(const QString& userId,
                                       const QString& displayName,
                                       const QString& avatarUrl = QString(),
                                       int role = 0,
                                       bool audioOn = true,
                                       bool videoOn = true,
                                       bool sharing = false);
    Q_INVOKABLE void removeParticipant(const QString& userId);
    Q_INVOKABLE void setHostUserId(const QString& userId);
    Q_INVOKABLE bool contains(const QString& userId) const;

    QStringList displayNames() const;
    QVector<ParticipantItem> items() const;
    ParticipantItem itemAt(int row) const;
    static ParticipantItem fromProto(const meeting::Participant& participant);

private:
    int indexOf(const QString& userId) const;
    void emitDataChangedForAll();

    QVector<ParticipantItem> m_items;
    QString m_hostUserId;
};
