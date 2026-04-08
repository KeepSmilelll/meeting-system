#pragma once

#include <QObject>
#include <QString>

class MeetingController;

class RuntimeSmokeDriver : public QObject {
    Q_OBJECT

public:
    explicit RuntimeSmokeDriver(MeetingController* controller, QObject* parent = nullptr);

    bool enabled() const;
    void start();

private:
    void handleInfoMessage(const QString& message);
    void handleLoggedInChanged();
    void handleInMeetingChanged();
    void handleStatusTextChanged();
    void pollMeetingId();
    void pollPeerSuccess();
    void completeSuccess(const QString& reason);
    void fail(const QString& reason);
    bool writeMeetingId(const QString& meetingId);
    void writeResult(const QString& status, const QString& reason) const;
    QString readMeetingId() const;
    QString readPeerResult() const;

    MeetingController* m_controller{nullptr};
    QString m_role;
    QString m_host;
    quint16 m_port{8443};
    QString m_username;
    QString m_password;
    QString m_title;
    QString m_meetingIdPath;
    QString m_resultPath;
    QString m_peerResultPath;
    QString m_pendingSuccessReason;
    bool m_enabled{false};
    bool m_startedLogin{false};
    bool m_startedCreate{false};
    bool m_startedJoin{false};
    bool m_reportedResult{false};
    bool m_waitingPeerSuccess{false};
};
