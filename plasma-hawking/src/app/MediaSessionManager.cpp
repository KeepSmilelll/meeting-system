#include "MediaSessionManager.h"

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
