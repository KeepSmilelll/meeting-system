#pragma once

#include <QAbstractItemModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

class QMediaDevices;
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
    Q_PROPERTY(QString activeVideoPeerUserId READ activeVideoPeerUserId NOTIFY activeVideoPeerUserIdChanged)
    Q_PROPERTY(QString activeShareDisplayName READ activeShareDisplayName NOTIFY activeShareChanged)
    Q_PROPERTY(bool hasActiveShare READ hasActiveShare NOTIFY activeShareChanged)
    Q_PROPERTY(QObject* localVideoFrameSource READ localVideoFrameSource NOTIFY localVideoFrameSourceChanged)
    Q_PROPERTY(QObject* remoteScreenFrameSource READ remoteScreenFrameSource NOTIFY remoteScreenFrameSourceChanged)
    Q_PROPERTY(QObject* remoteVideoFrameSource READ remoteVideoFrameSource NOTIFY remoteVideoFrameSourceChanged)
    Q_PROPERTY(QStringList availableCameraDevices READ availableCameraDevices NOTIFY availableCameraDevicesChanged)
    Q_PROPERTY(QString preferredCameraDevice READ preferredCameraDevice NOTIFY preferredCameraDeviceChanged)
    Q_PROPERTY(QStringList availableAudioInputDevices READ availableAudioInputDevices NOTIFY availableAudioInputDevicesChanged)
    Q_PROPERTY(QStringList availableAudioOutputDevices READ availableAudioOutputDevices NOTIFY availableAudioOutputDevicesChanged)
    Q_PROPERTY(QString preferredMicrophoneDevice READ preferredMicrophoneDevice NOTIFY preferredMicrophoneDeviceChanged)
    Q_PROPERTY(QString preferredSpeakerDevice READ preferredSpeakerDevice NOTIFY preferredSpeakerDeviceChanged)
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
    QString activeVideoPeerUserId() const;
    QString activeShareDisplayName() const;
    bool hasActiveShare() const;
    QObject* localVideoFrameSource() const;
    QObject* remoteScreenFrameSource() const;
    QObject* remoteVideoFrameSource() const;
    Q_INVOKABLE QObject* remoteVideoFrameSourceForUser(const QString& userId) const;
    Q_INVOKABLE quint32 remoteVideoSsrcForUser(const QString& userId) const;
    QStringList availableCameraDevices() const;
    QString preferredCameraDevice() const;
    QStringList availableAudioInputDevices() const;
    QStringList availableAudioOutputDevices() const;
    QString preferredMicrophoneDevice() const;
    QString preferredSpeakerDevice() const;
    QString username() const;
    QString userId() const;
    QString meetingId() const;
    QString meetingTitle() const;
    QAbstractItemModel* participantModel() const;
    QStringList participants() const;
    QString statusText() const;
    quint64 audioSentPacketCount() const;
    quint64 audioReceivedPacketCount() const;
    quint64 audioPlayedFrameCount() const;
    quint32 audioLastRttMs() const;
    quint32 audioTargetBitrateBps() const;
    bool audioIceConnected() const;
    bool audioDtlsConnected() const;
    bool audioSrtpReady() const;
    bool videoIceConnected() const;
    bool videoDtlsConnected() const;
    bool videoSrtpReady() const;
    qint64 videoLastAudioSkewMs() const;
    qint64 videoMaxAbsAudioSkewMs() const;
    quint64 videoAudioSkewSampleCount() const;
    quint64 videoAudioSkewCandidateCount() const;
    quint64 videoAudioSkewNoClockCount() const;
    quint64 videoAudioSkewInvalidVideoPtsCount() const;
    quint64 videoAudioSkewInvalidAudioClockCount() const;
    quint64 remoteVideoDecodedFrameCount() const;
    quint64 remoteVideoDroppedByAudioClockCount() const;
    quint64 remoteVideoQueuedFrameCount() const;
    quint64 remoteVideoRenderedFrameCount() const;
    quint64 remoteVideoStalePtsDropCount() const;
    quint64 remoteVideoRescheduledFrameCount() const;
    quint64 remoteVideoQueueResetCount() const;
    void shutdownMediaForRuntimeSmoke();

    Q_INVOKABLE void login(const QString& username, const QString& password);
    Q_INVOKABLE void setServerEndpoint(const QString& host, quint16 port);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void createMeeting(const QString& title, const QString& password, int maxParticipants = 16);
    Q_INVOKABLE void joinMeeting(const QString& meetingId, const QString& password);
    Q_INVOKABLE void leaveMeeting();
    Q_INVOKABLE void toggleAudio();
    Q_INVOKABLE void toggleVideo();
    Q_INVOKABLE void toggleScreenSharing();
    Q_INVOKABLE bool setPreferredCameraDevice(const QString& deviceName);
    Q_INVOKABLE bool setPreferredMicrophoneDevice(const QString& deviceName);
    Q_INVOKABLE bool setPreferredSpeakerDevice(const QString& deviceName);
    Q_INVOKABLE bool setActiveShareUserId(const QString& userId);
    Q_INVOKABLE bool setActiveVideoPeerUserId(const QString& userId);

signals:
    void loggedInChanged();
    void reconnectingChanged();
    void connectedChanged();
    void inMeetingChanged();
    void audioMutedChanged();
    void videoMutedChanged();
    void screenSharingChanged();
    void activeShareChanged();
    void activeVideoPeerUserIdChanged();
    void availableCameraDevicesChanged();
    void preferredCameraDeviceChanged();
    void availableAudioInputDevicesChanged();
    void availableAudioOutputDevicesChanged();
    void preferredMicrophoneDeviceChanged();
    void preferredSpeakerDeviceChanged();
    void localVideoFrameSourceChanged();
    void remoteScreenFrameSourceChanged();
    void remoteVideoFrameSourceChanged();
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
    struct PendingVideoRenderTask {
        qint64 dueAtMs{0};
        qint64 audioDelayDeadlineMs{0};
        int64_t videoPts90k{-1};
        uint64_t enqueueSeq{0};
        uint64_t ticket{0};
        QString peerUserId;
        std::function<void()> render;
    };

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
    QString currentAudioTransportKey() const;
    QString currentVideoTransportKey() const;
    quint32 currentVideoSsrc() const;
    QString resolvePeerUserIdForRemoteVideoSsrc(quint32 remoteSsrc) const;
    void updateRemoteVideoSsrcMappings();
    void updateExpectedRemoteVideoSsrcForCurrentPeer();
    void updateAudioSessionSettings();
    void updateVideoSessionSettings();
    void updateSfuRoute(const QString& route);
    bool resolveSfuEndpoint(QString* host, quint16* port) const;
    void applySfuRouteToSessions();
    void handleMediaTransportAnswer(const QString& meetingId,
                                    const QString& serverIceUfrag,
                                    const QString& serverIcePwd,
                                    const QString& serverDtlsFingerprint,
                                    const QStringList& serverCandidates,
                                    quint32 assignedAudioSsrc,
                                    quint32 assignedVideoSsrc);
    void updateActiveShareSelection();
    void updateActiveVideoPeerSelection();
    void enqueueRemoteVideoRenderTask(std::function<void()> renderTask,
                                      int renderDelayMs,
                                      int64_t videoPts90k,
                                      const QString& peerUserId);
    void drainRemoteVideoRenderQueue();
    void invalidateRemoteVideoRenderQueue(bool clearFrameStore);
    void resetRemoteVideoAvSyncStats();
    void resetAudioPeerState();
    void resetVideoPeerState(bool clearRemoteFrame);
    bool hasRemoteParticipant(const QString& userId) const;
    bool hasRemoteVideoParticipant(const QString& userId) const;
    void prunePeerNegotiationState();
    void syncLocalParticipantMediaState();
    void setScreenSharing(bool sharing);
    av::render::VideoFrameStore* ensureRemoteVideoFrameStore(const QString& userId);
    void clearRemoteVideoFrameStores();
    void refreshAvailableCameraDevices();
    void refreshAvailableAudioDevices();

    UserManager* m_userManager{nullptr};
    ParticipantListModel* m_participantModel{nullptr};
    std::unique_ptr<MeetingRepository> m_meetingRepository;
    std::unique_ptr<CallLogRepository> m_callLogRepository;
    std::unique_ptr<MediaSessionManager> m_audioSessionManager;
    std::unique_ptr<MediaSessionManager> m_mediaSessionManager;
    std::unique_ptr<av::session::AudioCallSession> m_audioCallSession;
    std::unique_ptr<av::session::ScreenShareSession> m_screenShareSession;
    std::unique_ptr<av::render::VideoFrameStore> m_localVideoFrameStore;
    std::unique_ptr<av::render::VideoFrameStore> m_remoteScreenFrameStore;
    std::unique_ptr<av::render::VideoFrameStore> m_remoteVideoFrameStore;
    QHash<QString, av::render::VideoFrameStore*> m_remoteVideoFrameStoresByPeer;
    signaling::SignalingClient* m_signaling{nullptr};
    signaling::Reconnector* m_reconnector{nullptr};
    QTimer* m_heartbeatTimer{nullptr};
    QTimer* m_videoRenderTimer{nullptr};
    QMediaDevices* m_mediaDevices{nullptr};

    bool m_loggedIn{false};
    bool m_reconnecting{false};
    bool m_inMeeting{false};
    bool m_audioMuted{false};
    bool m_videoMuted{true};
    bool m_screenSharing{false};
    bool m_waitingLeaveResponse{false};
    bool m_currentMeetingHost{false};
    bool m_audioNegotiationStarted{false};
    QString m_audioPeerUserId;
    QString m_audioTransportKey;
    bool m_videoNegotiationStarted{false};
    QString m_videoPeerUserId;
    QString m_videoTransportKey;
    QString m_activeShareUserId;
    QString m_activeVideoPeerUserId;
    QString m_activeShareDisplayName;
    QSet<QString> m_audioOfferSentPeers;
    QSet<QString> m_audioAnswerSentPeers;
    QSet<QString> m_videoOfferSentPeers;
    QSet<QString> m_videoAnswerSentPeers;
    QHash<QString, quint32> m_remoteVideoSsrcByPeer;
    quint32 m_assignedAudioSsrc{0};
    quint32 m_assignedVideoSsrc{0};
    QString m_audioClientIceUfrag;
    QString m_videoClientIceUfrag;
    QString m_meetingId;
    QString m_meetingTitle;
    QString m_sfuAddress;
    QStringList m_iceServers;
    QString m_pendingMeetingTitle;
    QString m_statusText{QStringLiteral("Ready")};
    qint64 m_currentMeetingJoinedAt{0};
    QString m_lastRouteStatusStage;
    QString m_lastRouteStatusMessage;
    qint64 m_lastRouteStatusAtMs{0};
    std::deque<PendingVideoRenderTask> m_remoteVideoRenderQueue;
    uint64_t m_videoRenderEnqueueSeq{0};
    uint64_t m_videoRenderTicket{0};
    qint64 m_lastVideoRenderAtMs{0};
    int64_t m_lastRenderedVideoPts90k{-1};
    qint64 m_lastVideoAudioSkewMs{0};
    qint64 m_maxAbsVideoAudioSkewMs{0};
    quint64 m_videoAudioSkewSampleCount{0};
    bool m_hasVideoAudioSkewSample{false};
    bool m_videoAudioSkewBaselineReady{false};
    qint64 m_videoAudioSkewBaselineVideoMs{0};
    qint64 m_videoAudioSkewBaselineAudioMs{0};
    quint64 m_videoAudioSkewCandidateCount{0};
    quint64 m_videoAudioSkewNoClockCount{0};
    quint64 m_videoAudioSkewInvalidVideoPtsCount{0};
    quint64 m_videoAudioSkewInvalidAudioClockCount{0};
    quint64 m_remoteVideoDecodedFrameCount{0};
    quint64 m_remoteVideoDroppedByAudioClockCount{0};
    quint64 m_remoteVideoQueuedFrameCount{0};
    quint64 m_remoteVideoRenderedFrameCount{0};
    quint64 m_remoteVideoStalePtsDropCount{0};
    quint64 m_remoteVideoRescheduledFrameCount{0};
    quint64 m_remoteVideoQueueResetCount{0};

    QString m_serverHost{QStringLiteral("127.0.0.1")};
    quint16 m_serverPort{8443};
    QString m_preferredCameraDevice;
    QStringList m_availableCameraDevices;
    QString m_preferredMicrophoneDevice;
    QString m_preferredSpeakerDevice;
    QStringList m_availableAudioInputDevices;
    QStringList m_availableAudioOutputDevices;
    QString m_username;
    QString m_userId;
    QString m_passwordHash;
    bool m_shouldStayConnected{false};
    bool m_waitingLogin{false};
    bool m_restoringSession{false};
};
