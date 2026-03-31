#include "MediaSessionManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

MediaSessionManager::MediaSessionManager(QObject* parent)
    : QObject(parent) {}

QString MediaSessionManager::localUserId() const {
    return m_localUserId;
}

QString MediaSessionManager::meetingId() const {
    return m_meetingId;
}

QString MediaSessionManager::localHost() const {
    return m_localHost;
}

quint16 MediaSessionManager::localPort() const {
    return m_localPort;
}

int MediaSessionManager::payloadType() const {
    return m_payloadType;
}

void MediaSessionManager::setLocalUserId(const QString& userId) {
    const QString normalized = userId.trimmed();
    if (m_localUserId == normalized) {
        return;
    }

    m_localUserId = normalized;
    emit localUserIdChanged();
}

void MediaSessionManager::setMeetingId(const QString& meetingId) {
    const QString normalized = meetingId.trimmed();
    if (m_meetingId == normalized) {
        return;
    }

    m_meetingId = normalized;
    emit meetingIdChanged();
}

void MediaSessionManager::setLocalHost(const QString& host) {
    const QString normalized = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    if (m_localHost == normalized) {
        return;
    }

    m_localHost = normalized;
    emit endpointChanged();
}

void MediaSessionManager::setLocalPort(quint16 port) {
    const quint16 normalized = port == 0 ? 5004 : port;
    if (m_localPort == normalized) {
        return;
    }

    m_localPort = normalized;
    emit endpointChanged();
}

void MediaSessionManager::setPayloadType(int payloadType) {
    if (payloadType <= 0 || m_payloadType == payloadType) {
        return;
    }

    m_payloadType = payloadType;
    emit endpointChanged();
}

void MediaSessionManager::reset() {
    emit negotiationStateChanged();
}

QString MediaSessionManager::buildOffer(const QString& peerUserId) const {
    return buildDescription(QStringLiteral("offer"), peerUserId);
}

QString MediaSessionManager::buildAnswer(const QString& peerUserId) const {
    return buildDescription(QStringLiteral("answer"), peerUserId);
}

bool MediaSessionManager::handleRemoteOffer(const QString& peerUserId, const QString& sdp) {
    QString host;
    quint16 port = 0;
    int payloadType = 0;
    QString remoteUserId;
    QString targetUserId;
    QString error;
    if (!parseDescription(sdp, host, port, payloadType, remoteUserId, targetUserId, error)) {
        Q_UNUSED(error)
        return false;
    }

    if (!m_localUserId.isEmpty() && !targetUserId.isEmpty() && targetUserId != m_localUserId) {
        return false;
    }

    const QString effectivePeer = peerUserId.trimmed().isEmpty() ? remoteUserId : peerUserId.trimmed();
    emit remoteEndpointReady(effectivePeer, host, port, payloadType, true);
    emit negotiationStateChanged();
    return true;
}

bool MediaSessionManager::handleRemoteAnswer(const QString& peerUserId, const QString& sdp) {
    QString host;
    quint16 port = 0;
    int payloadType = 0;
    QString remoteUserId;
    QString targetUserId;
    QString error;
    if (!parseDescription(sdp, host, port, payloadType, remoteUserId, targetUserId, error)) {
        Q_UNUSED(error)
        return false;
    }

    if (!m_localUserId.isEmpty() && !targetUserId.isEmpty() && targetUserId != m_localUserId) {
        return false;
    }

    const QString effectivePeer = peerUserId.trimmed().isEmpty() ? remoteUserId : peerUserId.trimmed();
    emit remoteEndpointReady(effectivePeer, host, port, payloadType, false);
    emit negotiationStateChanged();
    return true;
}
QString MediaSessionManager::buildDescription(const QString& kind, const QString& peerUserId) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("kind"), kind);
    obj.insert(QStringLiteral("meeting_id"), m_meetingId);
    obj.insert(QStringLiteral("local_user_id"), m_localUserId);
    obj.insert(QStringLiteral("peer_user_id"), peerUserId.trimmed());
    obj.insert(QStringLiteral("host"), m_localHost);
    obj.insert(QStringLiteral("port"), static_cast<int>(m_localPort));
    obj.insert(QStringLiteral("payload"), m_payloadType);
    obj.insert(QStringLiteral("transport"), QStringLiteral("udp"));

    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

bool MediaSessionManager::parseDescription(const QString& sdp,
                                          QString& host,
                                          quint16& port,
                                          int& payloadType,
                                          QString& localUserId,
                                          QString& targetUserId,
                                          QString& error) const {
    QJsonParseError jsonError{};
    const QJsonDocument doc = QJsonDocument::fromJson(sdp.toUtf8(), &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = jsonError.errorString();
        return false;
    }

    const QJsonObject obj = doc.object();
    const QString describedMeetingId = obj.value(QStringLiteral("meeting_id")).toString().trimmed();
    host = obj.value(QStringLiteral("host")).toString().trimmed();
    const int rawPort = obj.value(QStringLiteral("port")).toInt();
    payloadType = obj.value(QStringLiteral("payload")).toInt(m_payloadType);
    localUserId = obj.value(QStringLiteral("local_user_id")).toString().trimmed();
    targetUserId = obj.value(QStringLiteral("peer_user_id")).toString().trimmed();

    if (!m_meetingId.isEmpty() && !describedMeetingId.isEmpty() && describedMeetingId != m_meetingId) {
        error = QStringLiteral("Meeting mismatch");
        return false;
    }
    if (!m_localUserId.isEmpty() && !targetUserId.isEmpty() && targetUserId != m_localUserId) {
        error = QStringLiteral("Target user mismatch");
        return false;
    }
    if (localUserId.isEmpty()) {
        error = QStringLiteral("Missing remote user");
        return false;
    }
    if (host.isEmpty()) {
        error = QStringLiteral("Missing host");
        return false;
    }
    if (rawPort <= 0 || rawPort > 65535) {
        error = QStringLiteral("Invalid port");
        return false;
    }

    port = static_cast<quint16>(rawPort);
    if (payloadType <= 0) {
        payloadType = m_payloadType;
    }
    return true;
}