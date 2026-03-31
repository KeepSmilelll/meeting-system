#include "ParticipantListModel.h"

#include <QStringList>

#include "signaling.pb.h"

namespace {

ParticipantListModel::ParticipantItem itemFromDescriptor(const QString& descriptor) {
    ParticipantListModel::ParticipantItem item;
    const QString trimmed = descriptor.trimmed();
    if (trimmed.isEmpty()) {
        return item;
    }

    const int openParen = trimmed.lastIndexOf(QStringLiteral(" ("));
    if (openParen > 0 && trimmed.endsWith(QLatin1Char(')'))) {
        item.displayName = trimmed.left(openParen).trimmed();
        item.userId = trimmed.mid(openParen + 2, trimmed.size() - openParen - 3).trimmed();
    } else {
        item.displayName = trimmed;
        item.userId = trimmed;
    }

    item.role = 0;
    item.audioOn = true;
    item.videoOn = true;
    item.sharing = false;
    item.host = false;
    return item;
}

}  // namespace

ParticipantListModel::ParticipantListModel(QObject* parent)
    : QAbstractListModel(parent) {}

int ParticipantListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_items.size();
}

QVariant ParticipantListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) {
        return {};
    }

    const auto& item = m_items.at(index.row());
    switch (role) {
    case UserIdRole:
        return item.userId;
    case DisplayNameRole:
        return item.displayName;
    case AvatarUrlRole:
        return item.avatarUrl;
    case RoleRole:
        return item.role;
    case AudioOnRole:
        return item.audioOn;
    case VideoOnRole:
        return item.videoOn;
    case SharingRole:
        return item.sharing;
    case HostRole:
        return item.host;
    default:
        return {};
    }
}

QHash<int, QByteArray> ParticipantListModel::roleNames() const {
    static const QHash<int, QByteArray> roles{
        {UserIdRole, "userId"},
        {DisplayNameRole, "displayName"},
        {AvatarUrlRole, "avatarUrl"},
        {RoleRole, "role"},
        {AudioOnRole, "audioOn"},
        {VideoOnRole, "videoOn"},
        {SharingRole, "sharing"},
        {HostRole, "host"},
    };
    return roles;
}

void ParticipantListModel::clearParticipants() {
    if (m_items.isEmpty() && m_hostUserId.isEmpty()) {
        return;
    }

    beginResetModel();
    m_items.clear();
    m_hostUserId.clear();
    endResetModel();
}

void ParticipantListModel::replaceParticipantsFromDisplayList(const QStringList& participants) {
    QVector<ParticipantItem> items;
    items.reserve(participants.size());
    for (const QString& entry : participants) {
        items.append(itemFromDescriptor(entry));
    }
    replaceParticipants(items);
}

void ParticipantListModel::replaceParticipants(const QVector<ParticipantItem>& participants) {
    beginResetModel();
    m_items = participants;
    m_hostUserId.clear();
    for (const auto& item : m_items) {
        if (item.host || item.role == 1) {
            m_hostUserId = item.userId;
            break;
        }
    }
    for (auto& item : m_items) {
        item.host = !m_hostUserId.isEmpty() && item.userId == m_hostUserId;
    }
    endResetModel();
}

void ParticipantListModel::upsertParticipant(const QString& userId,
                                             const QString& displayName,
                                             const QString& avatarUrl,
                                             int role,
                                             bool audioOn,
                                             bool videoOn,
                                             bool sharing) {
    const QString trimmedUserId = userId.trimmed();
    if (trimmedUserId.isEmpty()) {
        return;
    }

    const int row = indexOf(trimmedUserId);
    ParticipantItem item;
    item.userId = trimmedUserId;
    item.displayName = displayName.trimmed().isEmpty() ? trimmedUserId : displayName.trimmed();
    item.avatarUrl = avatarUrl;
    item.role = role;
    item.audioOn = audioOn;
    item.videoOn = videoOn;
    item.sharing = sharing;
    item.host = (trimmedUserId == m_hostUserId) || role == 1;

    if (row >= 0) {
        m_items[row] = item;
        emit dataChanged(index(row, 0), index(row, 0), {UserIdRole, DisplayNameRole, AvatarUrlRole, RoleRole, AudioOnRole, VideoOnRole, SharingRole, HostRole});
    } else {
        const int insertRow = m_items.size();
        beginInsertRows(QModelIndex(), insertRow, insertRow);
        m_items.append(item);
        endInsertRows();
    }

    if (role == 1 || item.host) {
        setHostUserId(trimmedUserId);
    }
}

void ParticipantListModel::removeParticipant(const QString& userId) {
    const int row = indexOf(userId.trimmed());
    if (row < 0) {
        return;
    }

    const QString removedUserId = m_items.at(row).userId;
    const bool removedHost = removedUserId == m_hostUserId;

    beginRemoveRows(QModelIndex(), row, row);
    m_items.removeAt(row);
    endRemoveRows();

    if (removedHost) {
        m_hostUserId.clear();
        emitDataChangedForAll();
    }
}

void ParticipantListModel::setHostUserId(const QString& userId) {
    const QString trimmedUserId = userId.trimmed();
    if (m_hostUserId == trimmedUserId) {
        return;
    }

    m_hostUserId = trimmedUserId;
    emitDataChangedForAll();
}

bool ParticipantListModel::contains(const QString& userId) const {
    return indexOf(userId.trimmed()) >= 0;
}

QStringList ParticipantListModel::displayNames() const {
    QStringList names;
    names.reserve(m_items.size());
    for (const auto& item : m_items) {
        if (item.userId.isEmpty()) {
            names.push_back(item.displayName);
        } else {
            names.push_back(QStringLiteral("%1 (%2)").arg(item.displayName, item.userId));
        }
    }
    return names;
}

QVector<ParticipantListModel::ParticipantItem> ParticipantListModel::items() const {
    return m_items;
}

ParticipantListModel::ParticipantItem ParticipantListModel::itemAt(int row) const {
    if (row < 0 || row >= m_items.size()) {
        return {};
    }
    return m_items.at(row);
}

ParticipantListModel::ParticipantItem ParticipantListModel::fromProto(const meeting::Participant& participant) {
    ParticipantItem item;
    item.userId = QString::fromUtf8(participant.user_id().data(), static_cast<int>(participant.user_id().size()));
    item.displayName = QString::fromUtf8(participant.display_name().data(), static_cast<int>(participant.display_name().size()));
    if (item.displayName.trimmed().isEmpty()) {
        item.displayName = item.userId;
    }
    item.avatarUrl = QString::fromUtf8(participant.avatar_url().data(), static_cast<int>(participant.avatar_url().size()));
    item.role = participant.role();
    item.audioOn = participant.is_audio_on();
    item.videoOn = participant.is_video_on();
    item.sharing = participant.is_sharing();
    item.host = participant.role() == 1;
    return item;
}

int ParticipantListModel::indexOf(const QString& userId) const {
    const QString trimmedUserId = userId.trimmed();
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).userId == trimmedUserId) {
            return i;
        }
    }
    return -1;
}

void ParticipantListModel::emitDataChangedForAll() {
    if (m_items.isEmpty()) {
        return;
    }

    emit dataChanged(index(0, 0), index(m_items.size() - 1, 0), {RoleRole, HostRole});
}
