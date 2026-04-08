#include "MeetingController.h"

#include "ParticipantListModel.h"
#include "UserManager.h"
#include "net/signaling/Reconnector.h"
#include "net/signaling/SignalingClient.h"
#include "storage/CallLogRepository.h"
#include "storage/MeetingRepository.h"
#include "MediaSessionManager.h"
#include "av/session/AudioCallSession.h"
#include "av/session/ScreenShareSession.h"
#include "av/render/VideoFrameStore.h"
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTimer>
#include <QtGlobal>

#include <limits>
#include <memory>
#include "signaling.pb.h"

namespace {

constexpr quint16 kMeetMuteAll = 0x0209;
constexpr quint16 kMeetStateSync = 0x020B;
constexpr quint16 kMeetParticipantJoin = 0x020C;
constexpr quint16 kMeetParticipantLeave = 0x020D;
constexpr quint16 kMeetHostChanged = 0x020E;
constexpr quint16 kMediaRouteStatusNotify = 0x0306;
constexpr int kAudioPayloadType = 111;
constexpr int kScreenPayloadType = 97;
constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 1;
constexpr int kAudioFrameSamples = 960;
constexpr int kAudioBitrate = 32000;
constexpr int kScreenWidth = 1280;
constexpr int kScreenHeight = 720;
constexpr int kScreenFrameRate = 5;
constexpr int kScreenBitrate = 1500 * 1000;

QString toQtString(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

struct MediaRouteStatusEvent {
    QString stage;
    QString message;
};

MediaRouteStatusEvent parseMediaRouteStatusEvent(const QString& rawReason) {
    MediaRouteStatusEvent event;
    const QString trimmedReason = rawReason.trimmed();
    if (trimmedReason.isEmpty()) {
        return event;
    }

    const QJsonDocument jsonDoc = QJsonDocument::fromJson(trimmedReason.toUtf8());
    if (jsonDoc.isObject()) {
        const QJsonObject obj = jsonDoc.object();
        event.stage = obj.value(QStringLiteral("stage")).toString().trimmed().toLower();
        event.message = obj.value(QStringLiteral("message")).toString().trimmed();
    }

    if (event.message.isEmpty()) {
        event.message = trimmedReason;
    }
    if (event.stage.isEmpty()) {
        const QString lower = trimmedReason.toLower();
        if (lower.contains(QStringLiteral("switching"))) {
            event.stage = QStringLiteral("switching");
        } else if (lower.contains(QStringLiteral("failed"))) {
            event.stage = QStringLiteral("failed");
        } else if (lower.contains(QStringLiteral("switched"))) {
            event.stage = QStringLiteral("switched");
        } else {
            event.stage = QStringLiteral("info");
        }
    }
    return event;
}

QString resolveAdvertisedHost() {
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto& iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            !(iface.flags() & QNetworkInterface::IsRunning) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const auto& entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback()) {
                continue;
            }
            return addr.toString();
        }
    }
    return QStringLiteral("127.0.0.1");
}

bool shouldInitiateOffer(bool isHost, const QString& localUserId, const QString& peerUserId) {
    if (isHost) {
        return true;
    }
    if (localUserId.isEmpty() || peerUserId.isEmpty()) {
        return false;
    }
    return localUserId < peerUserId;
}

}  // namespace
MeetingController::MeetingController(const QString& databasePath, QObject* parent)
    : QObject(parent)
    , m_userManager(new UserManager(databasePath, this))
    , m_participantModel(new ParticipantListModel(this))
    , m_meetingRepository(std::make_unique<MeetingRepository>(databasePath))
    , m_callLogRepository(std::make_unique<CallLogRepository>(databasePath))
    , m_signaling(new signaling::SignalingClient(this))
    , m_reconnector(new signaling::Reconnector(this))
    , m_heartbeatTimer(new QTimer(this)) {
    m_reconnector->configure(1000, 30000);

    m_serverHost = m_userManager->serverHost();
    m_serverPort = m_userManager->serverPort();
    m_username = m_userManager->username();
    m_userId = m_userManager->userId();

    m_heartbeatTimer->setInterval(30 * 1000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (m_loggedIn) {
            m_signaling->sendHeartbeat(QDateTime::currentMSecsSinceEpoch());
        }
    });

    m_audioSessionManager = std::make_unique<MediaSessionManager>();
    m_mediaSessionManager = std::make_unique<MediaSessionManager>();
    m_remoteScreenFrameStore = std::make_unique<av::render::VideoFrameStore>();
    connect(m_audioSessionManager.get(), &MediaSessionManager::remoteEndpointReady, this,
            [this](const QString& peerUserId, const QString& host, quint16 port, int payloadType, bool offer) {
        if (!m_audioSessionManager || peerUserId.isEmpty()) {
            return;
        }

        if (!m_audioCallSession) {
            maybeStartAudioNegotiation();
        }
        updateAudioSessionSettings();

        if (m_audioCallSession && !host.isEmpty() && port != 0) {
            m_audioCallSession->setPeer(host.toStdString(), port);
            m_audioSessionManager->setLocalPort(m_audioCallSession->localPort());
        }

        m_audioPeerUserId = peerUserId;
        m_audioSessionManager->setPayloadType(payloadType > 0 ? payloadType : 111);

        if (offer) {
            if (!m_audioAnswerSent) {
                m_audioAnswerSent = true;
                m_audioNegotiationStarted = true;
                const QString answer = m_audioSessionManager->buildAnswer(peerUserId);
                const quint32 audioSsrc = m_audioCallSession ? m_audioCallSession->audioSsrc() : 0;
                m_signaling->sendMediaAnswer(peerUserId, answer, audioSsrc, 0);
                emit infoMessage(QStringLiteral("Audio answer sent to %1").arg(peerUserId));
            }
            return;
        }

        m_audioNegotiationStarted = true;
        emit infoMessage(QStringLiteral("Audio endpoint ready for %1").arg(peerUserId));
    });
    connect(m_mediaSessionManager.get(), &MediaSessionManager::remoteVideoEndpointReady, this,
            [this](const QString& peerUserId, const QString& host, quint16 port, int payloadType, bool offer) {
        Q_UNUSED(payloadType)
        if (host.isEmpty() || port == 0) {
            return;
        }

        if (!m_screenShareSession) {
            maybeStartVideoNegotiation();
        }
        if (!m_screenShareSession) {
            return;
        }

        m_videoPeerUserId = peerUserId;
        m_screenShareSession->setPeer(host.toStdString(), port);

        if (offer && !m_videoAnswerSent) {
            m_videoAnswerSent = true;
            m_videoNegotiationStarted = true;
            updateVideoSessionSettings();
            const QString answer = m_mediaSessionManager->buildAnswer(peerUserId);
            const quint32 videoSsrc = (m_screenSharing && m_screenShareSession) ? m_screenShareSession->videoSsrc() : 0;
            m_signaling->sendMediaAnswer(peerUserId, answer, 0, videoSsrc);
            emit infoMessage(QStringLiteral("Video answer sent to %1").arg(peerUserId));
            return;
        }

        m_videoNegotiationStarted = true;
        emit infoMessage(QStringLiteral("Video endpoint ready for %1").arg(peerUserId));
    });

    connect(m_signaling, &signaling::SignalingClient::connectedChanged, this, [this](bool connected) {
        if (connected) {
            if (m_reconnecting) {
                m_reconnecting = false;
                emit reconnectingChanged();
            }
            m_reconnector->reset();
            setStatusText(QStringLiteral("Connected"));
            emit connectedChanged();
            if (m_waitingLogin && !m_username.isEmpty()) {
                m_signaling->login(m_username, m_passwordHash, QStringLiteral("qt-client"), QStringLiteral("desktop"), m_restoringSession);
            }
            return;
        }

        const bool hadMeetingState = m_inMeeting || !m_meetingId.isEmpty() || !m_meetingTitle.isEmpty() || m_participantModel->rowCount() > 0;
        m_heartbeatTimer->stop();
        m_waitingLeaveResponse = false;
        if (hadMeetingState) {
            resetMeetingState(QStringLiteral("网络断开"));
            emit infoMessage(QStringLiteral("Connection lost, left the meeting"));
        }

        if (m_shouldStayConnected) {
            if (!m_reconnecting) {
                m_reconnecting = true;
                emit reconnectingChanged();
            }
            setStatusText(QStringLiteral("Reconnecting..."));
            m_reconnector->schedule();
        } else {
            setStatusText(QStringLiteral("Disconnected"));
            emit connectedChanged();
        }
    });

    connect(m_reconnector, &signaling::Reconnector::reconnectRequested, this, [this]() {
        m_signaling->reconnect();
    });

    connect(this, &MeetingController::infoMessage, this, [this](const QString& message) {
        qInfo().noquote() << "[meeting]" << message;
        setStatusText(message);
    });

    connect(m_signaling, &signaling::SignalingClient::protocolError, this, [this](const QString& message) {
        setStatusText(message);
        emit infoMessage(message);
    });

    connect(m_signaling, &signaling::SignalingClient::loginFinished, this,
            [this](bool success, const QString& userId, const QString& token, const QString& error) {
        const bool wasRestoringSession = m_restoringSession;
        m_waitingLogin = false;
        m_restoringSession = false;

        if (!success) {
            if (wasRestoringSession) {
                m_passwordHash.clear();
                m_userManager->clearToken();
            }

            const QString msg = wasRestoringSession
                                    ? QStringLiteral("Session expired, please login again")
                                    : (error.isEmpty() ? QStringLiteral("Login failed") : error);
            m_shouldStayConnected = false;
            m_reconnector->stop();
            if (m_reconnecting) {
                m_reconnecting = false;
                emit reconnectingChanged();
            }
            if (m_loggedIn) {
                m_loggedIn = false;
                emit loggedInChanged();
            }
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_userId = userId;
        m_passwordHash = token;
        m_userManager->setServerEndpoint(m_serverHost, m_serverPort);
        m_userManager->setSession(m_username, userId, token);
        emit sessionChanged();

        if (!m_loggedIn) {
            m_loggedIn = true;
            emit loggedInChanged();
        }
        m_shouldStayConnected = true;
        m_reconnector->reset();
        m_heartbeatTimer->start();
        setStatusText(QStringLiteral("Login success"));
        emit infoMessage(QStringLiteral("Login success"));
    });

    connect(m_signaling, &signaling::SignalingClient::heartbeatReceived, this, [this](qint64 serverTimestampMs) {
        Q_UNUSED(serverTimestampMs)
        if (m_loggedIn && m_statusText != QStringLiteral("Reconnecting...")) {
            setStatusText(QStringLiteral("Online"));
        }
    });

    connect(m_signaling, &signaling::SignalingClient::createMeetingFinished, this,
            [this](bool success, const QString& meetingId, const QString& error) {
        if (!success) {
            m_pendingMeetingTitle.clear();
            const QString msg = error.isEmpty() ? QStringLiteral("Create meeting failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_meetingId = meetingId;
        m_meetingTitle = m_pendingMeetingTitle;
        m_pendingMeetingTitle.clear();
        m_inMeeting = true;
        m_audioMuted = false;
        m_videoMuted = false;
        setScreenSharing(false);
        m_waitingLeaveResponse = false;
        m_currentMeetingHost = true;
        m_participantModel->clearParticipants();
        ensureLocalParticipant(true);
        persistMeetingSessionStart(true);
        syncParticipantsChanged();

        emit meetingIdChanged();
        emit meetingTitleChanged();
        emit inMeetingChanged();
        emit audioMutedChanged();
        emit videoMutedChanged();

        const QString msg = QStringLiteral("Meeting created: %1").arg(m_meetingId);
        setStatusText(msg);
        emit infoMessage(msg);
    });

    connect(m_signaling, &signaling::SignalingClient::joinMeetingFinished, this,
            [this](bool success, const QString& meetingId, const QString& title, const QStringList& participants, const QString& hostUserId, const QString& error) {
        if (!success) {
            const QString msg = error.isEmpty() ? QStringLiteral("Join meeting failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_meetingId = meetingId;
        m_meetingTitle = title;
        m_inMeeting = true;
        m_audioMuted = false;
        m_videoMuted = false;
        setScreenSharing(false);
        m_waitingLeaveResponse = false;
        const bool isHost = !hostUserId.isEmpty() && hostUserId == m_userId;
        m_currentMeetingHost = isHost;
        m_participantModel->replaceParticipantsFromDisplayList(participants);
        if (!hostUserId.isEmpty()) {
            m_participantModel->setHostUserId(hostUserId);
        }
        ensureLocalParticipant(isHost);
        persistMeetingSessionStart(isHost);
        syncParticipantsChanged();

        emit meetingIdChanged();
        emit meetingTitleChanged();
        emit inMeetingChanged();
        emit audioMutedChanged();
        emit videoMutedChanged();

        const QString msg = QStringLiteral("Joined meeting: %1").arg(m_meetingId);
        setStatusText(msg);
        emit infoMessage(msg);
    });

    connect(m_signaling, &signaling::SignalingClient::leaveMeetingFinished, this,
            [this](bool success, const QString& error) {
        const bool wasWaitingLeave = m_waitingLeaveResponse;
        m_waitingLeaveResponse = false;

        if (!success) {
            const QString msg = error.isEmpty() ? QStringLiteral("Leave meeting failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        if (!wasWaitingLeave && !m_inMeeting) {
            return;
        }

        resetMeetingState(QStringLiteral("主动离开"));
        setStatusText(QStringLiteral("Left meeting"));
        emit infoMessage(QStringLiteral("Left meeting"));
    });

    connect(m_signaling, &signaling::SignalingClient::kicked, this, [this](const QString& reason) {
        handleSessionKicked(reason);
    });

    connect(m_signaling, &signaling::SignalingClient::chatSendFinished, this,
            [this](bool success, const QString& messageId, const QString& error) {
        if (!success) {
            const QString msg = error.isEmpty() ? QStringLiteral("Send chat failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        emit infoMessage(QStringLiteral("Chat sent: %1").arg(messageId));
    });

    connect(m_signaling, &signaling::SignalingClient::chatReceived, this,
            [this](const QString& senderId,
                   const QString& senderName,
                   int type,
                   const QString& content,
                   const QString& replyToId,
                   qint64 timestamp) {
        Q_UNUSED(senderId)
        Q_UNUSED(type)
        Q_UNUSED(replyToId)
        Q_UNUSED(timestamp)
        emit infoMessage(QStringLiteral("[%1] %2").arg(senderName, content));
    });

    connect(m_signaling, &signaling::SignalingClient::mediaOfferReceived, this,
            [this](const QString& targetUserId, const QString& sdp) {
        if (!m_inMeeting || (!targetUserId.isEmpty() && targetUserId != m_userId)) {
            return;
        }

        maybeStartMediaNegotiation();
        if (!m_audioSessionManager || !m_mediaSessionManager) {
            return;
        }

        m_audioSessionManager->setLocalUserId(m_userId);
        m_audioSessionManager->setMeetingId(m_meetingId);
        m_mediaSessionManager->setLocalUserId(m_userId);
        m_mediaSessionManager->setMeetingId(m_meetingId);
        QString describedPeerUserId;
        bool hasAudio = false;
        bool hasVideo = false;
        if (!m_mediaSessionManager->inspectDescription(sdp, &describedPeerUserId, &hasAudio, &hasVideo)) {
            emit infoMessage(QStringLiteral("Failed to process media offer"));
            return;
        }

        const QString audioPeerUserId = currentAudioPeerUserId();
        const QString videoPeerUserId = currentVideoPeerUserId();
        const bool allowAudio = hasAudio && !audioPeerUserId.isEmpty() && describedPeerUserId == audioPeerUserId;
        const bool allowVideo = hasVideo && !videoPeerUserId.isEmpty() && describedPeerUserId == videoPeerUserId;

        const bool audioHandled = allowAudio && m_audioSessionManager->handleRemoteOffer(describedPeerUserId, sdp);
        const bool videoHandled = allowVideo && m_mediaSessionManager->handleRemoteOffer(describedPeerUserId, sdp);
        if (!audioHandled && !videoHandled) {
            emit infoMessage(QStringLiteral("Ignored media offer from unsubscribed peer %1").arg(describedPeerUserId));
            return;
        }
        updateAudioSessionSettings();
        updateVideoSessionSettings();
    });

    connect(m_signaling, &signaling::SignalingClient::mediaAnswerReceived, this,
            [this](const QString& targetUserId, const QString& sdp) {
        if (!m_inMeeting || (!targetUserId.isEmpty() && targetUserId != m_userId)) {
            return;
        }

        maybeStartMediaNegotiation();
        if (!m_audioSessionManager || !m_mediaSessionManager) {
            return;
        }

        m_audioSessionManager->setLocalUserId(m_userId);
        m_audioSessionManager->setMeetingId(m_meetingId);
        m_mediaSessionManager->setLocalUserId(m_userId);
        m_mediaSessionManager->setMeetingId(m_meetingId);
        QString describedPeerUserId;
        bool hasAudio = false;
        bool hasVideo = false;
        if (!m_mediaSessionManager->inspectDescription(sdp, &describedPeerUserId, &hasAudio, &hasVideo)) {
            emit infoMessage(QStringLiteral("Failed to process media answer"));
            return;
        }

        const QString audioPeerUserId = currentAudioPeerUserId();
        const QString videoPeerUserId = currentVideoPeerUserId();
        const bool allowAudio = hasAudio && !audioPeerUserId.isEmpty() && describedPeerUserId == audioPeerUserId;
        const bool allowVideo = hasVideo && !videoPeerUserId.isEmpty() && describedPeerUserId == videoPeerUserId;

        const bool audioHandled = allowAudio && m_audioSessionManager->handleRemoteAnswer(describedPeerUserId, sdp);
        const bool videoHandled = allowVideo && m_mediaSessionManager->handleRemoteAnswer(describedPeerUserId, sdp);
        if (!audioHandled && !videoHandled) {
            emit infoMessage(QStringLiteral("Ignored media answer from unsubscribed peer %1").arg(describedPeerUserId));
            return;
        }
        updateAudioSessionSettings();
        updateVideoSessionSettings();
    });
    connect(m_signaling, &signaling::SignalingClient::protobufMessageReceived, this,
            [this](quint16 signalType, const QByteArray& payload) {
        handleProtobufMessage(signalType, payload);
    });

    restoreCachedSession();
}

MeetingController::~MeetingController() = default;

bool MeetingController::loggedIn() const {
    return m_loggedIn;
}

bool MeetingController::reconnecting() const {
    return m_reconnecting;
}

bool MeetingController::connected() const {
    return m_signaling->isConnected();
}

bool MeetingController::inMeeting() const {
    return m_inMeeting;
}

bool MeetingController::audioMuted() const {
    return m_audioMuted;
}

bool MeetingController::videoMuted() const {
    return m_videoMuted;
}

bool MeetingController::screenSharing() const {
    return m_screenSharing;
}

QString MeetingController::activeAudioPeerUserId() const {
    return currentAudioPeerUserId();
}

QString MeetingController::activeShareUserId() const {
    return m_activeShareUserId;
}

QString MeetingController::activeShareDisplayName() const {
    return m_activeShareDisplayName;
}

bool MeetingController::hasActiveShare() const {
    return !m_activeShareUserId.isEmpty();
}

QObject* MeetingController::remoteScreenFrameSource() const {
    return m_remoteScreenFrameStore.get();
}

QString MeetingController::username() const {
    return m_username;
}

QString MeetingController::userId() const {
    return m_userId;
}

QString MeetingController::meetingId() const {
    return m_meetingId;
}

QString MeetingController::meetingTitle() const {
    return m_meetingTitle;
}

QAbstractItemModel* MeetingController::participantModel() const {
    return m_participantModel;
}

QStringList MeetingController::participants() const {
    return m_participantModel->displayNames();
}

QString MeetingController::statusText() const {
    return m_statusText;
}

void MeetingController::login(const QString& username, const QString& password) {
    if (username.trimmed().isEmpty() || password.isEmpty()) {
        setStatusText(QStringLiteral("Username/password required"));
        return;
    }

    m_username = username.trimmed();
    m_passwordHash = password;
    m_shouldStayConnected = true;
    m_waitingLogin = true;
    m_restoringSession = false;
    m_userManager->setServerEndpoint(m_serverHost, m_serverPort);
    emit sessionChanged();

    setStatusText(QStringLiteral("Connecting..."));

    if (m_signaling->isConnected()) {
        m_signaling->login(m_username, m_passwordHash, QStringLiteral("qt-client"), QStringLiteral("desktop"), m_restoringSession);
    } else {
        m_signaling->connectToServer(m_serverHost, m_serverPort);
    }
}

void MeetingController::setServerEndpoint(const QString& host, quint16 port) {
    const QString normalizedHost = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    const quint16 normalizedPort = port == 0 ? 8443 : port;
    if (m_serverHost == normalizedHost && m_serverPort == normalizedPort) {
        return;
    }

    m_serverHost = normalizedHost;
    m_serverPort = normalizedPort;
    if (m_userManager) {
        m_userManager->setServerEndpoint(m_serverHost, m_serverPort);
    }
}

void MeetingController::logout() {
    m_shouldStayConnected = false;
    m_waitingLogin = false;
    m_restoringSession = false;
    m_waitingLeaveResponse = false;
    m_pendingMeetingTitle.clear();
    m_reconnector->stop();
    m_heartbeatTimer->stop();
    resetMeetingState(QStringLiteral("主动登出"));
    m_passwordHash.clear();
    m_username.clear();
    m_userId.clear();
    m_userManager->clearSession();
    emit sessionChanged();

    if (m_reconnecting) {
        m_reconnecting = false;
        emit reconnectingChanged();
    }

    if (m_loggedIn) {
        m_loggedIn = false;
        emit loggedInChanged();
    }

    m_signaling->disconnectFromServer();
    setStatusText(QStringLiteral("Logged out"));
}

void MeetingController::createMeeting(const QString& title, const QString& password, int maxParticipants) {
    if (!m_loggedIn) {
        setStatusText(QStringLiteral("Please login first"));
        return;
    }

    m_pendingMeetingTitle = title.trimmed();
    if (m_pendingMeetingTitle.isEmpty()) {
        setStatusText(QStringLiteral("Meeting title required"));
        return;
    }

    const int boundedParticipants = qMax(1, maxParticipants);
    m_signaling->createMeeting(m_pendingMeetingTitle, password, boundedParticipants);
    setStatusText(QStringLiteral("Creating meeting..."));
}

void MeetingController::joinMeeting(const QString& meetingId, const QString& password) {
    if (!m_loggedIn) {
        setStatusText(QStringLiteral("Please login first"));
        return;
    }

    if (meetingId.trimmed().isEmpty()) {
        setStatusText(QStringLiteral("Meeting ID required"));
        return;
    }

    m_pendingMeetingTitle.clear();
    m_signaling->joinMeeting(meetingId.trimmed(), password);
    setStatusText(QStringLiteral("Joining meeting..."));
}

void MeetingController::leaveMeeting() {
    if (!m_inMeeting && m_meetingId.isEmpty() && m_participantModel->rowCount() == 0) {
        return;
    }

    if (m_waitingLeaveResponse) {
        return;
    }

    if (!m_signaling->isConnected()) {
        resetMeetingState(QStringLiteral("主动离开"));
        setStatusText(QStringLiteral("Left meeting"));
        emit infoMessage(QStringLiteral("Left meeting"));
        return;
    }

    m_waitingLeaveResponse = true;
    m_signaling->leaveMeeting();
    setStatusText(QStringLiteral("Leaving meeting..."));
}

void MeetingController::toggleAudio() {
    if (!m_inMeeting) {
        return;
    }
    m_audioMuted = !m_audioMuted;
    if (m_signaling->isConnected()) {
        m_signaling->sendMediaMuteToggle(0, m_audioMuted);
    }
    if (m_audioCallSession) {
        m_audioCallSession->setCaptureMuted(m_audioMuted);
    }
    if (!m_audioMuted) {
        maybeStartMediaNegotiation();
    }
    syncLocalParticipantMediaState();
    emit audioMutedChanged();
}

void MeetingController::toggleVideo() {
    if (!m_inMeeting) {
        return;
    }
    m_videoMuted = !m_videoMuted;
    if (m_signaling->isConnected()) {
        m_signaling->sendMediaMuteToggle(1, m_videoMuted);
    }
    syncLocalParticipantMediaState();
    emit videoMutedChanged();
}

void MeetingController::toggleScreenSharing() {
    if (!m_inMeeting) {
        return;
    }

    const bool nextSharing = !m_screenSharing;
    if (nextSharing) {
        setScreenSharing(true);
        maybeStartMediaNegotiation();
        if (m_screenShareSession && !m_screenShareSession->setSharingEnabled(true)) {
            setScreenSharing(false);
            emit infoMessage(QStringLiteral("Failed to start screen sharing"));
            return;
        }
    } else {
        if (m_screenShareSession && !m_screenShareSession->setSharingEnabled(false)) {
            emit infoMessage(QStringLiteral("Failed to stop screen sharing"));
            return;
        }
        setScreenSharing(false);
        maybeStartMediaNegotiation();
    }

    updateVideoSessionSettings();
    if (m_signaling->isConnected()) {
        m_signaling->sendMediaScreenShare(m_screenSharing);
    }
    syncLocalParticipantMediaState();
    m_videoOfferSent = false;
    m_videoAnswerSent = false;
    sendVideoOfferToPeer(true);
}

bool MeetingController::setActiveShareUserId(const QString& userId) {
    const QString normalized = userId.trimmed();
    if (normalized.isEmpty() || normalized == m_userId) {
        return false;
    }

    QString nextDisplayName;
    bool found = false;
    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId != normalized || !participant.sharing || participant.userId == m_userId) {
            continue;
        }

        nextDisplayName = participant.displayName.trimmed().isEmpty() ? participant.userId : participant.displayName;
        found = true;
        break;
    }

    if (!found || (m_activeShareUserId == normalized && m_activeShareDisplayName == nextDisplayName)) {
        return found;
    }

    m_activeShareUserId = normalized;
    m_activeShareDisplayName = nextDisplayName;
    emit activeShareChanged();

    maybeStartMediaNegotiation();
    sendVideoOfferToPeer(true);
    return true;
}

void MeetingController::setStatusText(const QString& text) {
    if (m_statusText == text) {
        return;
    }

    m_statusText = text;
    emit statusTextChanged();
}

void MeetingController::resetMeetingState(const QString& leaveReason) {
    resetMediaNegotiation();
    m_lastRouteStatusStage.clear();
    m_lastRouteStatusMessage.clear();
    m_lastRouteStatusAtMs = 0;
    if (!m_inMeeting && m_meetingId.isEmpty() && m_meetingTitle.isEmpty() && m_participantModel->rowCount() == 0) {
        m_waitingLeaveResponse = false;
        return;
    }

    const QString previousMeetingId = m_meetingId;
    const QString previousMeetingTitle = m_meetingTitle;
    const qint64 leftAt = QDateTime::currentMSecsSinceEpoch();
    if (!previousMeetingId.isEmpty()) {
        if (m_meetingRepository) {
            m_meetingRepository->markMeetingLeft(previousMeetingId, leftAt);
        }
        if (m_callLogRepository && m_currentMeetingJoinedAt > 0 && !m_userId.isEmpty()) {
            m_callLogRepository->finishActiveCall(previousMeetingId, m_userId, leaveReason, leftAt);
        }
    }

    const bool hadMeetingTitle = !previousMeetingTitle.isEmpty();
    m_inMeeting = false;
    m_audioMuted = false;
    m_videoMuted = false;
    setScreenSharing(false);
    m_waitingLeaveResponse = false;
    m_currentMeetingHost = false;
    m_currentMeetingJoinedAt = 0;
    m_meetingId.clear();
    m_meetingTitle.clear();
    m_participantModel->clearParticipants();
    syncParticipantsChanged();

    emit inMeetingChanged();
    emit audioMutedChanged();
    emit videoMutedChanged();
    emit meetingIdChanged();
    if (hadMeetingTitle) {
        emit meetingTitleChanged();
    }
}

void MeetingController::persistMeetingSessionStart(bool host) {
    if (m_meetingId.isEmpty()) {
        return;
    }

    const QString hostUserId = host ? m_userId : QString();
    if (m_currentMeetingJoinedAt > 0) {
        m_currentMeetingHost = m_currentMeetingHost || host;
        if (m_meetingRepository) {
            m_meetingRepository->upsertMeeting(m_meetingId, m_meetingTitle, m_currentMeetingHost ? m_userId : hostUserId, m_currentMeetingJoinedAt);
        }
        return;
    }

    const qint64 joinedAt = QDateTime::currentMSecsSinceEpoch();
    m_currentMeetingJoinedAt = joinedAt;
    m_currentMeetingHost = host;

    if (m_meetingRepository) {
        m_meetingRepository->upsertMeeting(m_meetingId, m_meetingTitle, hostUserId, joinedAt);
    }
    if (m_callLogRepository && !m_userId.isEmpty()) {
        m_callLogRepository->startCall(m_meetingId, m_meetingTitle, m_userId, joinedAt, host);
    }
}

void MeetingController::restoreCachedSession() {
    if (!m_userManager->hasCachedSession()) {
        return;
    }

    m_username = m_userManager->username();
    m_userId = m_userManager->userId();
    m_passwordHash = m_userManager->token();
    m_serverHost = m_userManager->serverHost();
    m_serverPort = m_userManager->serverPort();
    m_shouldStayConnected = true;
    m_waitingLogin = true;
    m_restoringSession = true;
    emit sessionChanged();

    setStatusText(QStringLiteral("Restoring session..."));
    m_signaling->connectToServer(m_serverHost, m_serverPort);
}

void MeetingController::handleProtobufMessage(quint16 signalType, const QByteArray& payload) {
    switch (signalType) {
    case kMeetStateSync: {
        meeting::MeetStateSyncNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetStateSyncNotify"));
            return;
        }

        QVector<ParticipantListModel::ParticipantItem> participants;
        participants.reserve(notify.participants_size());
        for (const auto& participant : notify.participants()) {
            participants.append(ParticipantListModel::fromProto(participant));
        }
        m_participantModel->replaceParticipants(participants);

        const QString meetingId = toQtString(notify.meeting_id());
        if (m_meetingId != meetingId) {
            m_meetingId = meetingId;
            emit meetingIdChanged();
        }

        const QString meetingTitle = toQtString(notify.title());
        if (m_meetingTitle != meetingTitle) {
            m_meetingTitle = meetingTitle;
            emit meetingTitleChanged();
        }

        m_waitingLeaveResponse = false;
        const QString hostId = toQtString(notify.host_id());
        m_participantModel->setHostUserId(hostId);
        m_currentMeetingHost = !hostId.isEmpty() && hostId == m_userId;
        ensureLocalParticipant(m_currentMeetingHost);
        for (const auto& participant : m_participantModel->items()) {
            if (participant.userId != m_userId) {
                continue;
            }

            const bool nextAudioMuted = !participant.audioOn;
            if (m_audioMuted != nextAudioMuted) {
                m_audioMuted = nextAudioMuted;
                emit audioMutedChanged();
            }

            const bool nextVideoMuted = !participant.videoOn;
            if (m_videoMuted != nextVideoMuted) {
                m_videoMuted = nextVideoMuted;
                emit videoMutedChanged();
            }

            if (m_audioCallSession) {
                m_audioCallSession->setCaptureMuted(m_audioMuted);
            }
            setScreenSharing(participant.sharing);
            if (m_screenShareSession) {
                m_screenShareSession->setSharingEnabled(m_screenSharing);
            }
            updateAudioSessionSettings();
            updateVideoSessionSettings();
            break;
        }
        if (!m_inMeeting) {
            m_inMeeting = true;
            emit inMeetingChanged();
        }
        syncParticipantsChanged();
        setStatusText(QStringLiteral("Meeting state synced"));
        emit infoMessage(QStringLiteral("Meeting state synced"));
        return;
    }
    case kMeetParticipantJoin: {
        meeting::MeetParticipantJoinNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetParticipantJoinNotify"));
            return;
        }

        const auto participant = ParticipantListModel::fromProto(notify.participant());
        m_participantModel->upsertParticipant(participant.userId,
                                              participant.displayName,
                                              participant.avatarUrl,
                                              participant.role,
                                              participant.audioOn,
                                              participant.videoOn,
                                              participant.sharing);
        if (participant.role == 1) {
            m_participantModel->setHostUserId(participant.userId);
            m_currentMeetingHost = participant.userId == m_userId;
        }
        if (!m_inMeeting) {
            m_inMeeting = true;
            emit inMeetingChanged();
        }
        syncParticipantsChanged();
        emit infoMessage(QStringLiteral("%1 joined").arg(participant.displayName.isEmpty() ? participant.userId : participant.displayName));
        return;
    }
    case kMeetParticipantLeave: {
        meeting::MeetParticipantLeaveNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetParticipantLeaveNotify"));
            return;
        }

        const QString userId = toQtString(notify.user_id());
        const QString reason = toQtString(notify.reason());
        if (!m_userId.isEmpty() && userId == m_userId) {
            const QString msg = reason.isEmpty() ? QStringLiteral("You left the meeting") : reason;
            resetMeetingState(reason);
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_participantModel->removeParticipant(userId);
        syncParticipantsChanged();
        emit infoMessage(reason.isEmpty() ? QStringLiteral("Participant left") : reason);
        return;
    }
    case kMeetHostChanged: {
        meeting::MeetHostChangedNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetHostChangedNotify"));
            return;
        }

        const QString newHostName = toQtString(notify.new_host_name());
        const QString newHostId = toQtString(notify.new_host_id());
        m_participantModel->setHostUserId(newHostId);
        m_currentMeetingHost = !newHostId.isEmpty() && newHostId == m_userId;
        syncParticipantsChanged();
        const QString msg = newHostName.isEmpty()
                                ? QStringLiteral("Host changed")
                                : QStringLiteral("Host changed to %1").arg(newHostName);
        setStatusText(msg);
        emit infoMessage(msg);
        return;
    }
    case kMeetMuteAll: {
        meeting::MeetMuteAllReq notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetMuteAllReq"));
            return;
        }
        if (!m_inMeeting) {
            return;
        }

        const bool muted = notify.mute();
        if (m_audioMuted != muted) {
            m_audioMuted = muted;
            emit audioMutedChanged();
        }
        if (m_audioCallSession) {
            m_audioCallSession->setCaptureMuted(m_audioMuted);
        }
        syncLocalParticipantMediaState();

        const QString msg = muted ? QStringLiteral("Host muted all") : QStringLiteral("Host unmuted all");
        setStatusText(msg);
        emit infoMessage(msg);
        return;
    }
    case kMediaRouteStatusNotify: {
        meeting::AuthKickNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MediaRouteStatusNotify"));
            return;
        }

        const MediaRouteStatusEvent event = parseMediaRouteStatusEvent(toQtString(notify.reason()));
        if (event.message.isEmpty()) {
            return;
        }

        setStatusText(event.message);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicate = (m_lastRouteStatusStage == event.stage && m_lastRouteStatusMessage == event.message);
        const qint64 elapsedMs = m_lastRouteStatusAtMs > 0 ? (nowMs - m_lastRouteStatusAtMs) : std::numeric_limits<qint64>::max();

        m_lastRouteStatusStage = event.stage;
        m_lastRouteStatusMessage = event.message;
        m_lastRouteStatusAtMs = nowMs;

        // "switching" is shown in status text only; toast-like info messages are reserved
        // for terminal states to avoid noisy UI hints under repeated retries.
        bool shouldEmitInfo = false;
        if (event.stage == QStringLiteral("failed") || event.stage == QStringLiteral("switched")) {
            shouldEmitInfo = !duplicate || elapsedMs > 3000;
        } else if (event.stage == QStringLiteral("info")) {
            shouldEmitInfo = !duplicate || elapsedMs > 3000;
        }
        if (shouldEmitInfo) {
            emit infoMessage(event.message);
        }
        return;
    }
    default:
        return;
    }
}

void MeetingController::ensureLocalParticipant(bool host) {
    if (m_userId.isEmpty()) {
        return;
    }

    ParticipantListModel::ParticipantItem existingItem;
    bool hasExistingItem = false;
    for (const auto& item : m_participantModel->items()) {
        if (item.userId == m_userId) {
            existingItem = item;
            hasExistingItem = true;
            break;
        }
    }

    const QString displayName = hasExistingItem
                                    ? existingItem.displayName
                                    : (m_username.isEmpty() ? QStringLiteral("You") : m_username);
    if (!hasExistingItem) {
        m_participantModel->upsertParticipant(m_userId,
                                              displayName,
                                              QString(),
                                              host ? 1 : 0,
                                              !m_audioMuted,
                                              !m_videoMuted,
                                              m_screenSharing);
    } else if (host && existingItem.role != 1) {
        m_participantModel->upsertParticipant(m_userId,
                                              displayName,
                                              existingItem.avatarUrl,
                                              1,
                                              existingItem.audioOn,
                                              existingItem.videoOn,
                                              existingItem.sharing);
    }

    if (host) {
        m_participantModel->setHostUserId(m_userId);
    }
}

void MeetingController::syncParticipantsChanged() {
    const QString previousActiveShareUserId = m_activeShareUserId;
    emit participantsChanged();
    updateActiveShareSelection();
    maybeStartMediaNegotiation();
    if (previousActiveShareUserId != m_activeShareUserId) {
        sendVideoOfferToPeer(true);
    }
}

void MeetingController::handleSessionKicked(const QString& reason) {
    const QString msg = reason.isEmpty() ? QStringLiteral("You were signed out") : reason;

    m_shouldStayConnected = false;
    m_waitingLogin = false;
    m_restoringSession = false;
    m_waitingLeaveResponse = false;
    m_pendingMeetingTitle.clear();
    m_reconnector->stop();
    m_heartbeatTimer->stop();
    resetMeetingState(msg);
    m_passwordHash.clear();
    m_userId.clear();
    m_userManager->clearToken();
    emit sessionChanged();

    if (m_reconnecting) {
        m_reconnecting = false;
        emit reconnectingChanged();
    }

    if (m_loggedIn) {
        m_loggedIn = false;
        emit loggedInChanged();
    }

    m_signaling->disconnectFromServer();
    setStatusText(msg);
    emit infoMessage(msg);
}


















QString MeetingController::currentAudioPeerUserId() const {
    if (m_userId.isEmpty()) {
        return {};
    }

    const auto participants = m_participantModel->items();
    for (const auto& participant : participants) {
        if (participant.userId == m_audioPeerUserId && participant.userId != m_userId) {
            return participant.userId;
        }
    }

    QString firstRemotePeerUserId;
    for (const auto& participant : participants) {
        if (participant.userId.isEmpty() || participant.userId == m_userId) {
            continue;
        }

        if (participant.host) {
            return participant.userId;
        }
        if (firstRemotePeerUserId.isEmpty()) {
            firstRemotePeerUserId = participant.userId;
        }
    }

    return firstRemotePeerUserId;
}

QString MeetingController::currentVideoPeerUserId() const {
    if (!m_activeShareUserId.isEmpty()) {
        return m_activeShareUserId;
    }

    if (m_screenSharing) {
        return currentAudioPeerUserId();
    }

    return {};
}

void MeetingController::updateAudioSessionSettings() {
    if (!m_audioSessionManager) {
        return;
    }

    m_audioSessionManager->setLocalUserId(m_userId);
    m_audioSessionManager->setMeetingId(m_meetingId);
    m_audioSessionManager->setLocalHost(resolveAdvertisedHost());
    m_audioSessionManager->setAudioPayloadType(kAudioPayloadType);
    m_audioSessionManager->setVideoPayloadType(kScreenPayloadType);
    m_audioSessionManager->setAudioNegotiationEnabled(true);
    m_audioSessionManager->setVideoNegotiationEnabled(false);
    if (m_audioCallSession) {
        m_audioSessionManager->setLocalAudioSsrc(m_audioCallSession->audioSsrc());
        const quint16 localPort = m_audioCallSession->localPort();
        if (localPort != 0) {
            m_audioSessionManager->setLocalAudioPort(localPort);
        }
    } else {
        m_audioSessionManager->setLocalAudioSsrc(0);
    }
    m_audioSessionManager->setLocalVideoSsrc(0);
}

void MeetingController::updateVideoSessionSettings() {
    if (!m_mediaSessionManager) {
        return;
    }

    m_mediaSessionManager->setLocalUserId(m_userId);
    m_mediaSessionManager->setMeetingId(m_meetingId);
    m_mediaSessionManager->setLocalHost(resolveAdvertisedHost());
    m_mediaSessionManager->setAudioPayloadType(kAudioPayloadType);
    m_mediaSessionManager->setVideoPayloadType(kScreenPayloadType);
    m_mediaSessionManager->setAudioNegotiationEnabled(false);
    m_mediaSessionManager->setVideoNegotiationEnabled(true);
    m_mediaSessionManager->setLocalAudioSsrc(0);
    if (m_screenShareSession && m_screenShareSession->isRunning()) {
        const quint16 videoPort = m_screenShareSession->localPort();
        if (videoPort != 0) {
            m_mediaSessionManager->setLocalVideoPort(videoPort);
        }
        m_mediaSessionManager->setLocalVideoSsrc(m_screenSharing ? m_screenShareSession->videoSsrc() : 0);
    } else {
        m_mediaSessionManager->setLocalVideoSsrc(0);
    }
}

void MeetingController::updateActiveShareSelection() {
    QString nextUserId;
    QString nextDisplayName;

    const auto participants = m_participantModel->items();
    for (const auto& participant : participants) {
        if (participant.userId == m_activeShareUserId && participant.userId != m_userId && participant.sharing) {
            nextUserId = participant.userId;
            nextDisplayName = participant.displayName.trimmed().isEmpty() ? participant.userId : participant.displayName;
            break;
        }
    }

    if (nextUserId.isEmpty()) {
        for (const auto& participant : participants) {
            if (participant.userId.isEmpty() || participant.userId == m_userId || !participant.sharing) {
                continue;
            }
            nextUserId = participant.userId;
            nextDisplayName = participant.displayName.trimmed().isEmpty() ? participant.userId : participant.displayName;
            break;
        }
    }

    if (m_activeShareUserId == nextUserId && m_activeShareDisplayName == nextDisplayName) {
        return;
    }

    m_activeShareUserId = nextUserId;
    m_activeShareDisplayName = nextDisplayName;
    emit activeShareChanged();
}

void MeetingController::resetAudioPeerState() {
    m_audioOfferSent = false;
    m_audioAnswerSent = false;
    m_audioNegotiationStarted = false;
    m_audioPeerUserId.clear();

    if (m_audioSessionManager) {
        m_audioSessionManager->reset();
    }
}

void MeetingController::resetVideoPeerState(bool clearRemoteFrame) {
    m_videoOfferSent = false;
    m_videoAnswerSent = false;
    m_videoNegotiationStarted = false;
    m_videoPeerUserId.clear();

    if (clearRemoteFrame && m_remoteScreenFrameStore) {
        m_remoteScreenFrameStore->clear();
    }

    if (m_mediaSessionManager) {
        m_mediaSessionManager->reset();
    }
}

void MeetingController::syncLocalParticipantMediaState() {
    if (m_userId.isEmpty()) {
        return;
    }

    ParticipantListModel::ParticipantItem existingItem;
    bool hasExistingItem = false;
    for (const auto& item : m_participantModel->items()) {
        if (item.userId == m_userId) {
            existingItem = item;
            hasExistingItem = true;
            break;
        }
    }

    if (!hasExistingItem && !m_inMeeting) {
        return;
    }

    const QString displayName = hasExistingItem
                                    ? existingItem.displayName
                                    : (m_username.isEmpty() ? QStringLiteral("You") : m_username);
    const QString avatarUrl = hasExistingItem ? existingItem.avatarUrl : QString();
    const int role = hasExistingItem ? existingItem.role : (m_currentMeetingHost ? 1 : 0);

    m_participantModel->upsertParticipant(m_userId,
                                          displayName,
                                          avatarUrl,
                                          role,
                                          !m_audioMuted,
                                          !m_videoMuted,
                                          m_screenSharing);
    if (m_currentMeetingHost || role == 1) {
        m_participantModel->setHostUserId(m_userId);
    }
}

void MeetingController::setScreenSharing(bool sharing) {
    if (m_screenSharing == sharing) {
        return;
    }

    m_screenSharing = sharing;
    emit screenSharingChanged();
}

void MeetingController::resetMediaNegotiation() {
    resetAudioPeerState();
    resetVideoPeerState(true);

    if (m_audioCallSession) {
        m_audioCallSession->stop();
        m_audioCallSession.reset();
    }

    if (m_screenShareSession) {
        m_screenShareSession->stop();
        m_screenShareSession.reset();
    }

}

void MeetingController::maybeStartMediaNegotiation() {
    maybeStartAudioNegotiation();
    maybeStartVideoNegotiation();
}

void MeetingController::maybeStartAudioNegotiation() {
    if (!m_loggedIn || !m_inMeeting || m_meetingId.isEmpty()) {
        return;
    }

    const QString peerUserId = currentAudioPeerUserId();
    if (peerUserId.isEmpty()) {
        resetAudioPeerState();
        if (m_audioCallSession) {
            m_audioCallSession->stop();
            m_audioCallSession.reset();
        }
        return;
    }

    if (!m_audioPeerUserId.isEmpty() && m_audioPeerUserId != peerUserId) {
        resetAudioPeerState();
    }

    if (!m_audioSessionManager) {
        m_audioSessionManager = std::make_unique<MediaSessionManager>();
    }

    if (!m_audioCallSession) {
        av::session::AudioCallSessionConfig config{};
        config.localAddress = "0.0.0.0";
        config.localPort = 0;
        config.peerAddress = "127.0.0.1";
        config.peerPort = 0;
        config.sampleRate = kAudioSampleRate;
        config.channels = kAudioChannels;
        config.frameSamples = kAudioFrameSamples;
        config.bitrate = kAudioBitrate;
        m_audioCallSession = std::make_unique<av::session::AudioCallSession>(config);
    }

    m_audioCallSession->setCaptureMuted(m_audioMuted);

    if (!m_audioCallSession->isRunning()) {
        if (!m_audioCallSession->start()) {
            emit infoMessage(QStringLiteral("Failed to start audio session: %1")
                                 .arg(QString::fromStdString(m_audioCallSession->lastError())));
            return;
        }
        m_audioNegotiationStarted = true;
    }

    m_audioPeerUserId = peerUserId;
    updateAudioSessionSettings();

    if (!m_signaling->isConnected()) {
        return;
    }

    sendAudioOfferToPeer(false);
}

void MeetingController::maybeStartVideoNegotiation() {
    if (!m_loggedIn || !m_inMeeting || m_meetingId.isEmpty()) {
        return;
    }

    const QString peerUserId = currentVideoPeerUserId();
    if (peerUserId.isEmpty()) {
        resetVideoPeerState(true);
        if (m_screenShareSession) {
            m_screenShareSession->stop();
            m_screenShareSession.reset();
        }
        return;
    }

    if (!m_videoPeerUserId.isEmpty() && m_videoPeerUserId != peerUserId) {
        resetVideoPeerState(false);
    }

    if (!m_mediaSessionManager) {
        m_mediaSessionManager = std::make_unique<MediaSessionManager>();
    }

    if (!m_screenShareSession) {
        av::session::ScreenShareSessionConfig config{};
        config.localAddress = "0.0.0.0";
        config.localPort = 0;
        config.peerAddress = "127.0.0.1";
        config.peerPort = 0;
        config.width = kScreenWidth;
        config.height = kScreenHeight;
        config.frameRate = kScreenFrameRate;
        config.bitrate = kScreenBitrate;
        config.payloadType = static_cast<uint8_t>(kScreenPayloadType);
        m_screenShareSession = std::make_unique<av::session::ScreenShareSession>(config);
        m_screenShareSession->setDecodedFrameCallback([this](av::codec::DecodedVideoFrame frame) {
            if (!m_remoteScreenFrameStore) {
                return;
            }
            QMetaObject::invokeMethod(this, [this, frame = std::move(frame)]() mutable {
                if (m_remoteScreenFrameStore) {
                    m_remoteScreenFrameStore->setFrame(std::move(frame));
                }
            }, Qt::QueuedConnection);
        });
    }

    if (m_screenShareSession && !m_screenShareSession->isRunning()) {
        if (!m_screenShareSession->start()) {
            emit infoMessage(QStringLiteral("Failed to start screen session: %1")
                                 .arg(QString::fromStdString(m_screenShareSession->lastError())));
            m_screenShareSession.reset();
        }
    }

    if (m_screenShareSession && m_screenSharing && !m_screenShareSession->setSharingEnabled(true)) {
        emit infoMessage(QStringLiteral("Failed to enable screen sharing: %1")
                             .arg(QString::fromStdString(m_screenShareSession->lastError())));
    }

    m_videoPeerUserId = peerUserId;
    updateVideoSessionSettings();

    if (!m_signaling->isConnected()) {
        return;
    }

    sendVideoOfferToPeer(false);
}

bool MeetingController::sendAudioOfferToPeer(bool force) {
    if (!m_audioSessionManager || !m_signaling->isConnected()) {
        return false;
    }

    const QString peerUserId = currentAudioPeerUserId();
    if (peerUserId.isEmpty()) {
        return false;
    }

    if (!force && (!shouldInitiateOffer(m_currentMeetingHost, m_userId, peerUserId) || m_audioOfferSent)) {
        return false;
    }

    updateAudioSessionSettings();
    const QString offer = m_audioSessionManager->buildOffer(peerUserId);
    if (offer.isEmpty()) {
        emit infoMessage(QStringLiteral("Failed to build audio offer"));
        return false;
    }

    const quint32 audioSsrc = m_audioCallSession ? m_audioCallSession->audioSsrc() : 0;
    m_signaling->sendMediaOffer(peerUserId, offer, audioSsrc, 0);
    m_audioOfferSent = true;
    m_audioAnswerSent = false;
    emit infoMessage(QStringLiteral("Audio offer sent to %1").arg(peerUserId));
    return true;
}

bool MeetingController::sendVideoOfferToPeer(bool force) {
    if (!m_mediaSessionManager || !m_signaling->isConnected()) {
        return false;
    }

    const QString peerUserId = currentVideoPeerUserId();
    if (peerUserId.isEmpty()) {
        return false;
    }

    if (!force && (!shouldInitiateOffer(m_currentMeetingHost, m_userId, peerUserId) || m_videoOfferSent)) {
        return false;
    }

    updateVideoSessionSettings();
    const QString offer = m_mediaSessionManager->buildOffer(peerUserId);
    if (offer.isEmpty()) {
        emit infoMessage(QStringLiteral("Failed to build video offer"));
        return false;
    }

    const quint32 videoSsrc = (m_screenSharing && m_screenShareSession) ? m_screenShareSession->videoSsrc() : 0;
    m_signaling->sendMediaOffer(peerUserId, offer, 0, videoSsrc);
    m_videoOfferSent = true;
    m_videoAnswerSent = false;
    emit infoMessage(QStringLiteral("Video offer sent to %1").arg(peerUserId));
    return true;
}












