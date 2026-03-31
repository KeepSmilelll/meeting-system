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

public slots:
    void setLocalUserId(const QString& userId);
    void setMeetingId(const QString& meetingId);
    void setLocalHost(const QString& host);
    void setLocalPort(quint16 port);
    void setPayloadType(int payloadType);
    void reset();

    QString buildOffer(const QString& peerUserId) const;
    QString buildAnswer(const QString& peerUserId) const;
    bool handleRemoteOffer(const QString& peerUserId, const QString& sdp);
    bool handleRemoteAnswer(const QString& peerUserId, const QString& sdp);

signals:
    void localUserIdChanged();
    void meetingIdChanged();
    void endpointChanged();
    void negotiationStateChanged();
    void remoteEndpointReady(const QString& peerUserId,
                             const QString& host,
                             quint16 port,
                             int payloadType,
                             bool offer);

private:
    QString buildDescription(const QString& kind, const QString& peerUserId) const;
    bool parseDescription(const QString& sdp,
                          QString& host,
                          quint16& port,
                          int& payloadType,
                          QString& localUserId,
                          QString& targetUserId,
                          QString& error) const;

    QString m_localUserId;
    QString m_meetingId;
    QString m_localHost{QStringLiteral("127.0.0.1")};
    quint16 m_localPort{5004};
    int m_payloadType{111};
};
