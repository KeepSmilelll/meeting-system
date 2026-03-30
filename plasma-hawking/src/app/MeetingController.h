#pragma once

#include <QObject>
#include <QString>

class MeetingController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool inMeeting READ inMeeting NOTIFY inMeetingChanged)
    Q_PROPERTY(bool audioMuted READ audioMuted NOTIFY audioMutedChanged)
    Q_PROPERTY(bool videoMuted READ videoMuted NOTIFY videoMutedChanged)
    Q_PROPERTY(QString meetingId READ meetingId NOTIFY meetingIdChanged)

public:
    explicit MeetingController(QObject* parent = nullptr);

    bool inMeeting() const;
    bool audioMuted() const;
    bool videoMuted() const;
    QString meetingId() const;

    Q_INVOKABLE void createMeeting(const QString& title, const QString& password);
    Q_INVOKABLE void joinMeeting(const QString& meetingId, const QString& password);
    Q_INVOKABLE void leaveMeeting();
    Q_INVOKABLE void toggleAudio();
    Q_INVOKABLE void toggleVideo();

signals:
    void inMeetingChanged();
    void audioMutedChanged();
    void videoMutedChanged();
    void meetingIdChanged();
    void infoMessage(const QString& message);

private:
    bool m_inMeeting{false};
    bool m_audioMuted{false};
    bool m_videoMuted{false};
    QString m_meetingId;
};
