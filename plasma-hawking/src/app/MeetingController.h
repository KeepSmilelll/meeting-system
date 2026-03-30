#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

namespace signaling {
class Reconnector;
class SignalingClient;
}

class MeetingController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY reconnectingChanged)
    Q_PROPERTY(bool inMeeting READ inMeeting NOTIFY inMeetingChanged)
    Q_PROPERTY(bool audioMuted READ audioMuted NOTIFY audioMutedChanged)
    Q_PROPERTY(bool videoMuted READ videoMuted NOTIFY videoMutedChanged)
    Q_PROPERTY(QString meetingId READ meetingId NOTIFY meetingIdChanged)
    Q_PROPERTY(QStringList participants READ participants NOTIFY participantsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    explicit MeetingController(QObject* parent = nullptr);

    bool loggedIn() const;
    bool reconnecting() const;
    bool inMeeting() const;
    bool audioMuted() const;
    bool videoMuted() const;
    QString meetingId() const;
    QStringList participants() const;
    QString statusText() const;

    Q_INVOKABLE void login(const QString& username, const QString& password);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void createMeeting(const QString& title, const QString& password);
    Q_INVOKABLE void joinMeeting(const QString& meetingId, const QString& password);
    Q_INVOKABLE void leaveMeeting();
    Q_INVOKABLE void toggleAudio();
    Q_INVOKABLE void toggleVideo();

signals:
    void loggedInChanged();
    void reconnectingChanged();
    void inMeetingChanged();
    void audioMutedChanged();
    void videoMutedChanged();
    void meetingIdChanged();
    void participantsChanged();
    void statusTextChanged();
    void infoMessage(const QString& message);

private:
    void setStatusText(const QString& text);
    void resetMeetingState();

    signaling::SignalingClient* m_signaling{nullptr};
    signaling::Reconnector* m_reconnector{nullptr};
    QTimer* m_heartbeatTimer{nullptr};

    bool m_loggedIn{false};
    bool m_reconnecting{false};
    bool m_inMeeting{false};
    bool m_audioMuted{false};
    bool m_videoMuted{false};
    QString m_meetingId;
    QStringList m_participants;
    QString m_statusText{QStringLiteral("Ready")};

    QString m_serverHost{QStringLiteral("127.0.0.1")};
    quint16 m_serverPort{8443};
    QString m_username;
    QString m_passwordHash;
    bool m_shouldStayConnected{false};
    bool m_waitingLogin{false};
};
