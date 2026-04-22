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
    return m_localAudioPort;
}

int MediaSessionManager::payloadType() const {
    return m_audioPayloadType;
}

quint16 MediaSessionManager::localAudioPort() const {
    return m_localAudioPort;
}

int MediaSessionManager::audioPayloadType() const {
    return m_audioPayloadType;
}

quint16 MediaSessionManager::localVideoPort() const {
    return m_localVideoPort;
}

int MediaSessionManager::videoPayloadType() const {
    return m_videoPayloadType;
}

quint32 MediaSessionManager::localAudioSsrc() const {
    return m_localAudioSsrc;
}

quint32 MediaSessionManager::localVideoSsrc() const {
    return m_localVideoSsrc;
}

bool MediaSessionManager::audioNegotiationEnabled() const {
    return m_audioNegotiationEnabled;
}

bool MediaSessionManager::videoNegotiationEnabled() const {
    return m_videoNegotiationEnabled;
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
    setLocalAudioPort(port);
}

void MediaSessionManager::setLocalAudioPort(quint16 port) {
    const quint16 normalized = port == 0 ? 5004 : port;
    if (m_localAudioPort == normalized) {
        return;
    }

    m_localAudioPort = normalized;
    emit endpointChanged();
}

void MediaSessionManager::setPayloadType(int payloadType) {
    setAudioPayloadType(payloadType);
}

void MediaSessionManager::setAudioPayloadType(int payloadType) {
    if (payloadType <= 0 || m_audioPayloadType == payloadType) {
        return;
    }

    m_audioPayloadType = payloadType;
    emit endpointChanged();
}

void MediaSessionManager::setLocalVideoPort(quint16 port) {
    const quint16 normalized = port == 0 ? 5006 : port;
    if (m_localVideoPort == normalized) {
        return;
    }

    m_localVideoPort = normalized;
    emit endpointChanged();
}

void MediaSessionManager::setVideoPayloadType(int payloadType) {
    if (payloadType <= 0 || m_videoPayloadType == payloadType) {
        return;
    }

    m_videoPayloadType = payloadType;
    emit endpointChanged();
}

void MediaSessionManager::setLocalAudioSsrc(quint32 ssrc) {
    if (m_localAudioSsrc == ssrc) {
        return;
    }

    m_localAudioSsrc = ssrc;
    emit endpointChanged();
}

void MediaSessionManager::setLocalVideoSsrc(quint32 ssrc) {
    if (m_localVideoSsrc == ssrc) {
        return;
    }

    m_localVideoSsrc = ssrc;
    emit endpointChanged();
}

void MediaSessionManager::setAudioNegotiationEnabled(bool enabled) {
    if (m_audioNegotiationEnabled == enabled) {
        return;
    }

    m_audioNegotiationEnabled = enabled;
    emit endpointChanged();
}

void MediaSessionManager::setVideoNegotiationEnabled(bool enabled) {
    if (m_videoNegotiationEnabled == enabled) {
        return;
    }

    m_videoNegotiationEnabled = enabled;
    emit endpointChanged();
}

void MediaSessionManager::reset() {
    m_localAudioSsrc = 0;
    m_localVideoSsrc = 0;
    m_audioNegotiationEnabled = true;
    m_videoNegotiationEnabled = true;
    emit negotiationStateChanged();
}

QString MediaSessionManager::buildOffer(const QString& peerUserId) const {
    return buildDescription(QStringLiteral("offer"), peerUserId);
}

QString MediaSessionManager::buildAnswer(const QString& peerUserId) const {
    return buildDescription(QStringLiteral("answer"), peerUserId);
}

bool MediaSessionManager::inspectDescription(const QString& sdp, QString* remoteUserId, bool* hasAudio, bool* hasVideo) const {
    QString audioHost;
    quint16 audioPort = 0;
    int audioPayloadType = 0;
    QString videoHost;
    quint16 videoPort = 0;
    int videoPayloadType = 0;
    QString parsedRemoteUserId;
    QString targetUserId;
    QString error;
    const bool parsed = parseDescription(sdp,
                                         audioHost,
                                         audioPort,
                                         audioPayloadType,
                                         videoHost,
                                         videoPort,
                                         videoPayloadType,
                                         parsedRemoteUserId,
                                         targetUserId,
                                         error);
    if (!parsed) {
        Q_UNUSED(error)
        return false;
    }

    if (remoteUserId != nullptr) {
        *remoteUserId = parsedRemoteUserId;
    }
    if (hasAudio != nullptr) {
        *hasAudio = !audioHost.isEmpty() && audioPort != 0;
    }
    if (hasVideo != nullptr) {
        *hasVideo = !videoHost.isEmpty() && videoPort != 0;
    }
    return true;
}

bool MediaSessionManager::handleRemoteOffer(const QString& peerUserId, const QString& sdp) {
    QString audioHost;
    quint16 audioPort = 0;
    int audioPayloadType = 0;
    QString videoHost;
    quint16 videoPort = 0;
    int videoPayloadType = 0;
    QString remoteUserId;
    QString targetUserId;
    QString error;
    if (!parseDescription(sdp,
                          audioHost,
                          audioPort,
                          audioPayloadType,
                          videoHost,
                          videoPort,
                          videoPayloadType,
                          remoteUserId,
                          targetUserId,
                          error)) {
        Q_UNUSED(error)
        return false;
    }

    if (!m_localUserId.isEmpty() && !targetUserId.isEmpty() && targetUserId != m_localUserId) {
        return false;
    }

    const QString effectivePeer = peerUserId.trimmed().isEmpty() ? remoteUserId : peerUserId.trimmed();
    if (!audioHost.isEmpty() && audioPort != 0) {
        emit remoteEndpointReady(effectivePeer, audioHost, audioPort, audioPayloadType, true);
    }
    if (!videoHost.isEmpty() && videoPort != 0) {
        emit remoteVideoEndpointReady(effectivePeer, videoHost, videoPort, videoPayloadType, true);
    }
    emit negotiationStateChanged();
    return true;
}

bool MediaSessionManager::handleRemoteAnswer(const QString& peerUserId, const QString& sdp) {
    QString audioHost;
    quint16 audioPort = 0;
    int audioPayloadType = 0;
    QString videoHost;
    quint16 videoPort = 0;
    int videoPayloadType = 0;
    QString remoteUserId;
    QString targetUserId;
    QString error;
    if (!parseDescription(sdp,
                          audioHost,
                          audioPort,
                          audioPayloadType,
                          videoHost,
                          videoPort,
                          videoPayloadType,
                          remoteUserId,
                          targetUserId,
                          error)) {
        Q_UNUSED(error)
        return false;
    }

    if (!m_localUserId.isEmpty() && !targetUserId.isEmpty() && targetUserId != m_localUserId) {
        return false;
    }

    const QString effectivePeer = peerUserId.trimmed().isEmpty() ? remoteUserId : peerUserId.trimmed();
    if (!audioHost.isEmpty() && audioPort != 0) {
        emit remoteEndpointReady(effectivePeer, audioHost, audioPort, audioPayloadType, false);
    }
    if (!videoHost.isEmpty() && videoPort != 0) {
        emit remoteVideoEndpointReady(effectivePeer, videoHost, videoPort, videoPayloadType, false);
    }
    emit negotiationStateChanged();
    return true;
}
QString MediaSessionManager::buildDescription(const QString& kind, const QString& peerUserId) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("kind"), kind);
    obj.insert(QStringLiteral("meeting_id"), m_meetingId);
    obj.insert(QStringLiteral("local_user_id"), m_localUserId);
    obj.insert(QStringLiteral("peer_user_id"), peerUserId.trimmed());
    obj.insert(QStringLiteral("transport"), QStringLiteral("udp"));

    if (m_audioNegotiationEnabled) {
        obj.insert(QStringLiteral("host"), m_localHost);
        obj.insert(QStringLiteral("port"), static_cast<int>(m_localAudioPort));
        obj.insert(QStringLiteral("payload"), m_audioPayloadType);

        QJsonObject audio;
        audio.insert(QStringLiteral("host"), m_localHost);
        audio.insert(QStringLiteral("port"), static_cast<int>(m_localAudioPort));
        audio.insert(QStringLiteral("payload"), m_audioPayloadType);
        if (m_localAudioSsrc != 0) {
            obj.insert(QStringLiteral("audio_ssrc"), static_cast<qint64>(m_localAudioSsrc));
            audio.insert(QStringLiteral("ssrc"), static_cast<qint64>(m_localAudioSsrc));
        }
        obj.insert(QStringLiteral("audio"), audio);
    }

    if (m_videoNegotiationEnabled && m_localVideoPort != 0) {
        QJsonObject video;
        video.insert(QStringLiteral("host"), m_localHost);
        video.insert(QStringLiteral("port"), static_cast<int>(m_localVideoPort));
        video.insert(QStringLiteral("payload"), m_videoPayloadType);
        video.insert(QStringLiteral("source"), m_videoPayloadType == 97 ? QStringLiteral("screen") : QStringLiteral("camera"));
        if (m_localVideoSsrc != 0) {
            obj.insert(QStringLiteral("video_ssrc"), static_cast<qint64>(m_localVideoSsrc));
            video.insert(QStringLiteral("ssrc"), static_cast<qint64>(m_localVideoSsrc));
        }
        obj.insert(QStringLiteral("video"), video);
    }

    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

bool MediaSessionManager::parseDescription(const QString& sdp,
                                          QString& audioHost,
                                          quint16& audioPort,
                                          int& audioPayloadType,
                                          QString& videoHost,
                                          quint16& videoPort,
                                          int& videoPayloadType,
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
    const QJsonObject audioObject = obj.value(QStringLiteral("audio")).toObject();
    const QJsonObject videoObject = obj.value(QStringLiteral("video")).toObject();
    const bool hasAudioObject = !audioObject.isEmpty();
    const bool hasVideoObject = !videoObject.isEmpty();
    const QString describedMeetingId = obj.value(QStringLiteral("meeting_id")).toString().trimmed();
    int rawAudioPort = 0;
    int rawVideoPort = 0;

    if (hasAudioObject) {
        audioHost = audioObject.value(QStringLiteral("host")).toString().trimmed();
        if (audioHost.isEmpty()) {
            audioHost = obj.value(QStringLiteral("host")).toString().trimmed();
        }
        rawAudioPort = audioObject.contains(QStringLiteral("port"))
                           ? audioObject.value(QStringLiteral("port")).toInt()
                           : obj.value(QStringLiteral("port")).toInt();
        audioPayloadType = audioObject.contains(QStringLiteral("payload"))
                               ? audioObject.value(QStringLiteral("payload")).toInt(m_audioPayloadType)
                               : obj.value(QStringLiteral("payload")).toInt(m_audioPayloadType);
    }

    if (hasVideoObject) {
        videoHost = videoObject.value(QStringLiteral("host")).toString().trimmed();
        if (videoHost.isEmpty()) {
            videoHost = audioHost.isEmpty() ? obj.value(QStringLiteral("host")).toString().trimmed() : audioHost;
        }
        rawVideoPort = videoObject.value(QStringLiteral("port")).toInt();
        videoPayloadType = videoObject.value(QStringLiteral("payload")).toInt(m_videoPayloadType);
    }

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

    if (!hasAudioObject && !hasVideoObject) {
        error = QStringLiteral("Missing media sections");
        return false;
    }

    if (hasAudioObject) {
        if (audioHost.isEmpty()) {
            error = QStringLiteral("Missing host");
            return false;
        }
        if (rawAudioPort <= 0 || rawAudioPort > 65535) {
            error = QStringLiteral("Invalid port");
            return false;
        }
        audioPort = static_cast<quint16>(rawAudioPort);
        if (audioPayloadType <= 0) {
            audioPayloadType = m_audioPayloadType;
        }
    } else {
        audioHost.clear();
        audioPort = 0;
        audioPayloadType = m_audioPayloadType;
    }

    if (hasVideoObject) {
        if (videoHost.isEmpty()) {
            error = QStringLiteral("Missing video host");
            return false;
        }
        if (rawVideoPort <= 0 || rawVideoPort > 65535) {
            error = QStringLiteral("Invalid video port");
            return false;
        }
        videoPort = static_cast<quint16>(rawVideoPort);
        if (videoPayloadType <= 0) {
            videoPayloadType = m_videoPayloadType;
        }
    } else {
        videoHost.clear();
        videoPort = 0;
        videoPayloadType = m_videoPayloadType;
    }
    return true;
}
