#include "MeetingController.h"

#include "ParticipantListModel.h"
#include "UserManager.h"
#include "net/signaling/Reconnector.h"
#include "net/signaling/SignalingClient.h"
#include "storage/CallLogRepository.h"
#include "storage/MeetingRepository.h"
#include "MediaSessionManager.h"
#include "av/session/AudioCallSession.h"
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QTimer>
#include <QtGlobal>

#include <memory>
#include "signaling.pb.h"

namespace {

constexpr quint16 kMeetStateSync = 0x020B;
constexpr quint16 kMeetParticipantJoin = 0x020C;
constexpr quint16 kMeetParticipantLeave = 0x020D;
constexpr quint16 kMeetHostChanged = 0x020E;
constexpr int kAudioPayloadType = 111;
constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 1;
constexpr int kAudioFrameSamples = 960;
constexpr int kAudioBitrate = 32000;

QString toQtString(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
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
MeetingController::MeetingController(QObject* parent)
    : QObject(parent)
    , m_userManager(new UserManager(QString(), this))
    , m_participantModel(new ParticipantListModel(this))
    , m_meetingRepository(std::make_unique<MeetingRepository>())
    , m_callLogRepository(std::make_unique<CallLogRepository>())
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

    m_mediaSessionManager = std::make_unique<MediaSessionManager>();
    connect(m_mediaSessionManager.get(), &MediaSessionManager::remoteEndpointReady, this,
            [this](const QString& peerUserId, const QString& host, quint16 port, int payloadType, bool offer) {
        if (!m_mediaSessionManager || peerUserId.isEmpty()) {
            return;
        }

        if (!m_audioCallSession) {
            maybeStartMediaNegotiation();
        }
        updateMediaSessionSettings();

        if (m_audioCallSession && !host.isEmpty() && port != 0) {
            m_audioCallSession->setPeer(host.toStdString(), port);
            m_mediaSessionManager->setLocalPort(m_audioCallSession->localPort());
        }

        m_mediaPeerUserId = peerUserId;
        m_mediaSessionManager->setPayloadType(payloadType > 0 ? payloadType : 111);

        if (offer) {
            if (!m_mediaAnswerSent) {
                m_mediaAnswerSent = true;
                m_mediaNegotiationStarted = true;
                const QString answer = m_mediaSessionManager->buildAnswer(peerUserId);
                m_signaling->sendMediaAnswer(peerUserId, answer);
                emit infoMessage(QStringLiteral("Media answer sent to %1").arg(peerUserId));
            }
            return;
        }

        m_mediaNegotiationStarted = true;
        emit infoMessage(QStringLiteral("Media endpoint ready for %1").arg(peerUserId));
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
        if (!m_mediaSessionManager) {
            return;
        }

        m_mediaSessionManager->setLocalUserId(m_userId);
        m_mediaSessionManager->setMeetingId(m_meetingId);
        if (!m_mediaSessionManager->handleRemoteOffer(QString(), sdp)) {
            emit infoMessage(QStringLiteral("Failed to process media offer"));
            return;
        }
        updateMediaSessionSettings();
    });

    connect(m_signaling, &signaling::SignalingClient::mediaAnswerReceived, this,
            [this](const QString& targetUserId, const QString& sdp) {
        if (!m_inMeeting || (!targetUserId.isEmpty() && targetUserId != m_userId)) {
            return;
        }

        maybeStartMediaNegotiation();
        if (!m_mediaSessionManager) {
            return;
        }

        m_mediaSessionManager->setLocalUserId(m_userId);
        m_mediaSessionManager->setMeetingId(m_meetingId);
        if (!m_mediaSessionManager->handleRemoteAnswer(QString(), sdp)) {
            emit infoMessage(QStringLiteral("Failed to process media answer"));
            return;
        }
        updateMediaSessionSettings();
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
    if (m_audioCallSession) {
        m_audioCallSession->setCaptureMuted(m_audioMuted);
    }
    if (!m_audioMuted) {
        maybeStartMediaNegotiation();
    }
    emit audioMutedChanged();
}

void MeetingController::toggleVideo() {
    if (!m_inMeeting) {
        return;
    }
    m_videoMuted = !m_videoMuted;
    emit videoMutedChanged();
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
        ensureLocalParticipant(false);
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
    default:
        return;
    }
}

void MeetingController::ensureLocalParticipant(bool host) {
    if (m_userId.isEmpty()) {
        return;
    }

    const QString displayName = m_username.isEmpty() ? QStringLiteral("You") : m_username;
    const int role = host ? 1 : 0;
    if (!m_participantModel->contains(m_userId)) {
        m_participantModel->upsertParticipant(m_userId, displayName, QString(), role, true, true, false);
    } else if (host) {
        m_participantModel->upsertParticipant(m_userId, displayName, QString(), role, true, true, false);
    }

    if (host) {
        m_participantModel->setHostUserId(m_userId);
    }
}

void MeetingController::syncParticipantsChanged() {
    emit participantsChanged();
    maybeStartMediaNegotiation();
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


















QString MeetingController::currentPeerUserId() const {
    if (m_userId.isEmpty()) {
        return {};
    }

    QString peerUserId;
    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId.isEmpty() || participant.userId == m_userId) {
            continue;
        }

        if (!peerUserId.isEmpty() && peerUserId != participant.userId) {
            return {};
        }

        peerUserId = participant.userId;
    }

    return peerUserId;
}

void MeetingController::updateMediaSessionSettings() {
    if (!m_mediaSessionManager) {
        return;
    }

    m_mediaSessionManager->setLocalUserId(m_userId);
    m_mediaSessionManager->setMeetingId(m_meetingId);
    m_mediaSessionManager->setLocalHost(resolveAdvertisedHost());
    m_mediaSessionManager->setPayloadType(kAudioPayloadType);
    if (m_audioCallSession) {
        const quint16 localPort = m_audioCallSession->localPort();
        if (localPort != 0) {
            m_mediaSessionManager->setLocalPort(localPort);
        }
    }
}

void MeetingController::resetMediaNegotiation() {
    m_mediaOfferSent = false;
    m_mediaAnswerSent = false;
    m_mediaNegotiationStarted = false;
    m_mediaPeerUserId.clear();

    if (m_audioCallSession) {
        m_audioCallSession->stop();
        m_audioCallSession.reset();
    }

    if (m_mediaSessionManager) {
        m_mediaSessionManager->reset();
    }
}

void MeetingController::maybeStartMediaNegotiation() {
    if (!m_loggedIn || !m_inMeeting || m_meetingId.isEmpty()) {
        return;
    }

    const QString peerUserId = currentPeerUserId();
    if (peerUserId.isEmpty()) {
        resetMediaNegotiation();
        return;
    }

    if (!m_mediaSessionManager) {
        m_mediaSessionManager = std::make_unique<MediaSessionManager>();
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
        m_mediaNegotiationStarted = true;
    }

    m_mediaPeerUserId = peerUserId;
    updateMediaSessionSettings();

    if (!m_signaling->isConnected()) {
        return;
    }

    if (shouldInitiateOffer(m_currentMeetingHost, m_userId, peerUserId) && !m_mediaOfferSent) {
        const QString offer = m_mediaSessionManager->buildOffer(peerUserId);
        if (offer.isEmpty()) {
            emit infoMessage(QStringLiteral("Failed to build media offer"));
            return;
        }

        m_signaling->sendMediaOffer(peerUserId, offer);
        m_mediaOfferSent = true;
        m_mediaAnswerSent = false;
        emit infoMessage(QStringLiteral("Media offer sent to %1").arg(peerUserId));
    }
}








