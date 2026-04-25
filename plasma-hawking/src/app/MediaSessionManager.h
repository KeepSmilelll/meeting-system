#pragma once

#include <QObject>
#include <QString>

class MediaSessionManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString localUserId READ localUserId WRITE setLocalUserId NOTIFY localUserIdChanged)
    Q_PROPERTY(QString meetingId READ meetingId WRITE setMeetingId NOTIFY meetingIdChanged)
    Q_PROPERTY(QString localHost READ localHost WRITE setLocalHost NOTIFY endpointChanged)
    Q_PROPERTY(quint16 localPort READ localPort WRITE setLocalPort NOTIFY endpointChanged)
    Q_PROPERTY(int payloadType READ payloadType WRITE setPayloadType NOTIFY endpointChanged)

public:
    explicit MediaSessionManager(QObject* parent = nullptr);

    QString localUserId() const;
    QString meetingId() const;
    QString localHost() const;
    quint16 localPort() const;
    int payloadType() const;
    quint16 localAudioPort() const;
    int audioPayloadType() const;
    quint16 localVideoPort() const;
    int videoPayloadType() const;
    quint32 localAudioSsrc() const;
    quint32 localVideoSsrc() const;
    bool audioNegotiationEnabled() const;
    bool videoNegotiationEnabled() const;

public slots:
    void setLocalUserId(const QString& userId);
    void setMeetingId(const QString& meetingId);
    void setLocalHost(const QString& host);
    void setLocalPort(quint16 port);
    void setPayloadType(int payloadType);
    void setLocalAudioPort(quint16 port);
    void setAudioPayloadType(int payloadType);
    void setLocalVideoPort(quint16 port);
    void setVideoPayloadType(int payloadType);
    void setLocalAudioSsrc(quint32 ssrc);
    void setLocalVideoSsrc(quint32 ssrc);
    void setAudioNegotiationEnabled(bool enabled);
    void setVideoNegotiationEnabled(bool enabled);
    void reset();

signals:
    void localUserIdChanged();
    void meetingIdChanged();
    void endpointChanged();
    void negotiationStateChanged();

private:
    QString m_localUserId;
    QString m_meetingId;
    QString m_localHost{QStringLiteral("127.0.0.1")};
    quint16 m_localAudioPort{5004};
    int m_audioPayloadType{111};
    quint16 m_localVideoPort{5006};
    int m_videoPayloadType{97};
    quint32 m_localAudioSsrc{0};
    quint32 m_localVideoSsrc{0};
    bool m_audioNegotiationEnabled{true};
    bool m_videoNegotiationEnabled{true};
};

