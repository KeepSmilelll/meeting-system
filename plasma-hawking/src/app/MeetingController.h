#pragma once

#include <QAbstractItemModel>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

class QTimer;
class UserManager;
class ParticipantListModel;
class MeetingRepository;
class CallLogRepository;
class MediaSessionManager;
namespace av::session {
class AudioCallSession;
class ScreenShareSession;
}
namespace av::render {
class VideoFrameStore;
}

namespace signaling {
class Reconnector;
class SignalingClient;
}

class MeetingController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY reconnectingChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool inMeeting READ inMeeting NOTIFY inMeetingChanged)
    Q_PROPERTY(bool audioMuted READ audioMuted NOTIFY audioMutedChanged)
    Q_PROPERTY(bool videoMuted READ videoMuted NOTIFY videoMutedChanged)
    Q_PROPERTY(bool screenSharing READ screenSharing NOTIFY screenSharingChanged)
    Q_PROPERTY(QString activeShareUserId READ activeShareUserId NOTIFY activeShareChanged)
    Q_PROPERTY(QString activeShareDisplayName READ activeShareDisplayName NOTIFY activeShareChanged)
    Q_PROPERTY(bool hasActiveShare READ hasActiveShare NOTIFY activeShareChanged)
    Q_PROPERTY(QObject* remoteScreenFrameSource READ remoteScreenFrameSource NOTIFY remoteScreenFrameSourceChanged)
    Q_PROPERTY(QString username READ username NOTIFY sessionChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY sessionChanged)
    Q_PROPERTY(QString meetingId READ meetingId NOTIFY meetingIdChanged)
    Q_PROPERTY(QString meetingTitle READ meetingTitle NOTIFY meetingTitleChanged)
    Q_PROPERTY(QAbstractItemModel* participantModel READ participantModel CONSTANT)
    Q_PROPERTY(QStringList participants READ participants NOTIFY participantsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    explicit MeetingController(const QString& databasePath = QString(), QObject* parent = nullptr);
    ~MeetingController() override;

    bool loggedIn() const;
    bool reconnecting() const;
    bool connected() const;
    bool inMeeting() const;
    bool audioMuted() const;
    bool videoMuted() const;
    bool screenSharing() const;
    QString activeAudioPeerUserId() const;
    QString activeShareUserId() const;
    QString activeShareDisplayName() const;
    bool hasActiveShare() const;
    QObject* remoteScreenFrameSource() const;
    QString username() const;
    QString userId() const;
    QString meetingId() const;
    QString meetingTitle() const;
    QAbstractItemModel* participantModel() const;
    QStringList participants() const;
    QString statusText() const;

    Q_INVOKABLE void login(const QString& username, const QString& password);
    Q_INVOKABLE void setServerEndpoint(const QString& host, quint16 port);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void createMeeting(const QString& title, const QString& password, int maxParticipants = 16);
    Q_INVOKABLE void joinMeeting(const QString& meetingId, const QString& password);
    Q_INVOKABLE void leaveMeeting();
    Q_INVOKABLE void toggleAudio();
    Q_INVOKABLE void toggleVideo();
    Q_INVOKABLE void toggleScreenSharing();
    Q_INVOKABLE bool setActiveShareUserId(const QString& userId);

signals:
    void loggedInChanged();
    void reconnectingChanged();
    void connectedChanged();
    void inMeetingChanged();
    void audioMutedChanged();
    void videoMutedChanged();
    void screenSharingChanged();
    void activeShareChanged();
    void remoteScreenFrameSourceChanged();
    void sessionChanged();
    void meetingIdChanged();
    void meetingTitleChanged();
    void participantsChanged();
    void statusTextChanged();
    void infoMessage(const QString& message);
    void mediaEndpointReady(const QString& peerUserId,
                            const QString& host,
                            quint16 port,
                            int payloadType,
                            bool offer);

private:
    void setStatusText(const QString& text);
    void resetMeetingState(const QString& leaveReason = QString());
    void persistMeetingSessionStart(bool host);
    void restoreCachedSession();
    void handleProtobufMessage(quint16 signalType, const QByteArray& payload);
    void ensureLocalParticipant(bool host);
    void syncParticipantsChanged();
    void handleSessionKicked(const QString& reason);
    void maybeStartMediaNegotiation();
    void maybeStartAudioNegotiation();
    void maybeStartVideoNegotiation();
    void resetMediaNegotiation();
    bool sendAudioOfferToPeer(bool force);
    bool sendVideoOfferToPeer(bool force);
    QString currentAudioPeerUserId() const;
    QString currentVideoPeerUserId() const;
    void updateAudioSessionSettings();
    void updateVideoSessionSettings();
    void updateActiveShareSelection();
    void resetAudioPeerState();
    void resetVideoPeerState(bool clearRemoteFrame);
    void syncLocalParticipantMediaState();
    void setScreenSharing(bool sharing);

    UserManager* m_userManager{nullptr};
    ParticipantListModel* m_participantModel{nullptr};
    std::unique_ptr<MeetingRepository> m_meetingRepository;
    std::unique_ptr<CallLogRepository> m_callLogRepository;
    std::unique_ptr<MediaSessionManager> m_audioSessionManager;
    std::unique_ptr<MediaSessionManager> m_mediaSessionManager;
    std::unique_ptr<av::session::AudioCallSession> m_audioCallSession;
    std::unique_ptr<av::session::ScreenShareSession> m_screenShareSession;
    std::unique_ptr<av::render::VideoFrameStore> m_remoteScreenFrameStore;
    signaling::SignalingClient* m_signaling{nullptr};
    signaling::Reconnector* m_reconnector{nullptr};
    QTimer* m_heartbeatTimer{nullptr};

    bool m_loggedIn{false};
    bool m_reconnecting{false};
    bool m_inMeeting{false};
    bool m_audioMuted{false};
    bool m_videoMuted{false};
    bool m_screenSharing{false};
    bool m_waitingLeaveResponse{false};
    bool m_currentMeetingHost{false};
    bool m_audioNegotiationStarted{false};
    bool m_audioOfferSent{false};
    bool m_audioAnswerSent{false};
    QString m_audioPeerUserId;
    bool m_videoNegotiationStarted{false};
    bool m_videoOfferSent{false};
    bool m_videoAnswerSent{false};
    QString m_videoPeerUserId;
    QString m_activeShareUserId;
    QString m_activeShareDisplayName;
    QString m_meetingId;
    QString m_meetingTitle;
    QString m_pendingMeetingTitle;
    QString m_statusText{QStringLiteral("Ready")};
    qint64 m_currentMeetingJoinedAt{0};
    QString m_lastRouteStatusStage;
    QString m_lastRouteStatusMessage;
    qint64 m_lastRouteStatusAtMs{0};

    QString m_serverHost{QStringLiteral("127.0.0.1")};
    quint16 m_serverPort{8443};
    QString m_username;
    QString m_userId;
    QString m_passwordHash;
    bool m_shouldStayConnected{false};
    bool m_waitingLogin{false};
    bool m_restoringSession{false};
};

