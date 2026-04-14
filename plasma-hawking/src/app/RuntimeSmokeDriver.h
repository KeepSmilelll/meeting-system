#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QString>

class MeetingController;

class RuntimeSmokeDriver : public QObject {
    Q_OBJECT

public:
    explicit RuntimeSmokeDriver(MeetingController* controller, QObject* parent = nullptr);

    bool enabled() const;
    void start();

private:
    QString currentStageTag() const;
    QString videoPipelineSummary() const;
    QString audioPipelineSummary() const;
    QString withStageTag(const QString& reason) const;
    void handleInfoMessage(const QString& message);
    void handleLoggedInChanged();
    void handleInMeetingChanged();
    void handleStatusTextChanged();
    void pollMeetingId();
    void pollPeerSuccess();
    void maybeUpdateVideoEvidence();
    void maybeUpdateAudioEvidence();
    void maybeCompleteSuccess(const QString& reason);
    void maybeStartSoak(const QString& reason);
    void pollSoakProgress();
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
    QString m_videoEncodeDetail;
    bool m_enabled{false};
    bool m_startedLogin{false};
    bool m_startedCreate{false};
    bool m_startedJoin{false};
    bool m_appliedLocalVideoPolicy{false};
    bool m_reportedResult{false};
    bool m_waitingPeerSuccess{false};
    bool m_peerSuccessObserved{false};
    bool m_requireVideoEvidence{false};
    bool m_requireAudioEvidence{false};
    bool m_disableLocalVideo{false};
    QString m_expectedCameraSource;
    bool m_cameraSourceObserved{false};
    bool m_videoCameraFrameObserved{false};
    bool m_videoPeerConfiguredObserved{false};
    bool m_videoEncodedPacketObserved{false};
    bool m_videoRtpSentObserved{false};
    bool m_videoRtpReceivedObserved{false};
    bool m_videoFrameDecodedObserved{false};
    bool m_videoEvidenceReady{false};
    bool m_audioEvidenceReady{false};
    int m_soakDurationMs{0};
    int m_soakPollIntervalMs{1000};
    bool m_soakStarted{false};
    QElapsedTimer m_soakTimer;
};

