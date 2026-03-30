#include "MeetingController.h"

#include "net/signaling/Reconnector.h"
#include "net/signaling/SignalingClient.h"

#include <QDateTime>
#include <QTimer>

MeetingController::MeetingController(QObject* parent)
    : QObject(parent)
    , m_signaling(new signaling::SignalingClient(this))
    , m_reconnector(new signaling::Reconnector(this))
    , m_heartbeatTimer(new QTimer(this)) {
    m_reconnector->configure(1000, 30000);

    m_heartbeatTimer->setInterval(30 * 1000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (m_loggedIn) {
            m_signaling->sendHeartbeat(QDateTime::currentMSecsSinceEpoch());
        }
    });

    connect(m_signaling, &signaling::SignalingClient::connectedChanged, this, [this](bool connected) {
        if (connected) {
            if (m_reconnecting) {
                m_reconnecting = false;
                emit reconnectingChanged();
            }
            m_reconnector->reset();
            setStatusText(QStringLiteral("Connected"));
            if (m_waitingLogin && !m_username.isEmpty()) {
                m_signaling->login(m_username, m_passwordHash, QStringLiteral("qt-client"), QStringLiteral("desktop"));
            }
            return;
        }

        m_heartbeatTimer->stop();
        if (m_shouldStayConnected) {
            if (!m_reconnecting) {
                m_reconnecting = true;
                emit reconnectingChanged();
            }
            setStatusText(QStringLiteral("Reconnecting..."));
            m_reconnector->schedule();
        } else {
            setStatusText(QStringLiteral("Disconnected"));
        }
    });

    connect(m_reconnector, &signaling::Reconnector::reconnectRequested, this, [this]() {
        m_signaling->reconnect();
    });

    connect(m_signaling, &signaling::SignalingClient::protocolError, this, [this](const QString& message) {
        setStatusText(message);
        emit infoMessage(message);
    });

    connect(m_signaling, &signaling::SignalingClient::loginFinished, this,
            [this](bool success, const QString& userId, const QString& token, const QString& error) {
        Q_UNUSED(userId)
        Q_UNUSED(token)

        m_waitingLogin = false;

        if (!success) {
            const QString msg = error.isEmpty() ? QStringLiteral("Login failed") : error;
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

        if (!m_loggedIn) {
            m_loggedIn = true;
            emit loggedInChanged();
        }
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
            const QString msg = error.isEmpty() ? QStringLiteral("Create meeting failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_meetingId = meetingId;
        m_inMeeting = true;
        m_audioMuted = false;
        m_videoMuted = false;
        m_participants = {QStringLiteral("You")};

        emit meetingIdChanged();
        emit inMeetingChanged();
        emit audioMutedChanged();
        emit videoMutedChanged();
        emit participantsChanged();

        const QString msg = QStringLiteral("Meeting created: %1").arg(m_meetingId);
        setStatusText(msg);
        emit infoMessage(msg);
    });

    connect(m_signaling, &signaling::SignalingClient::joinMeetingFinished, this,
            [this](bool success, const QString& meetingId, const QString& title, const QStringList& participants, const QString& error) {
        Q_UNUSED(title)

        if (!success) {
            const QString msg = error.isEmpty() ? QStringLiteral("Join meeting failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_meetingId = meetingId;
        m_inMeeting = true;
        m_audioMuted = false;
        m_videoMuted = false;
        m_participants = participants;
        if (m_participants.isEmpty()) {
            m_participants = {QStringLiteral("You")};
        }

        emit meetingIdChanged();
        emit inMeetingChanged();
        emit audioMutedChanged();
        emit videoMutedChanged();
        emit participantsChanged();

        const QString msg = QStringLiteral("Joined meeting: %1").arg(m_meetingId);
        setStatusText(msg);
        emit infoMessage(msg);
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
}

bool MeetingController::loggedIn() const {
    return m_loggedIn;
}

bool MeetingController::reconnecting() const {
    return m_reconnecting;
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

QString MeetingController::meetingId() const {
    return m_meetingId;
}

QStringList MeetingController::participants() const {
    return m_participants;
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

    setStatusText(QStringLiteral("Connecting..."));

    if (m_signaling->isConnected()) {
        m_signaling->login(m_username, m_passwordHash, QStringLiteral("qt-client"), QStringLiteral("desktop"));
    } else {
        m_signaling->connectToServer(m_serverHost, m_serverPort);
    }
}

void MeetingController::logout() {
    m_shouldStayConnected = false;
    m_waitingLogin = false;
    m_reconnector->stop();
    m_heartbeatTimer->stop();

    if (m_reconnecting) {
        m_reconnecting = false;
        emit reconnectingChanged();
    }

    if (m_loggedIn) {
        m_loggedIn = false;
        emit loggedInChanged();
    }

    resetMeetingState();
    m_signaling->disconnectFromServer();
    setStatusText(QStringLiteral("Logged out"));
}

void MeetingController::createMeeting(const QString& title, const QString& password) {
    if (!m_loggedIn) {
        setStatusText(QStringLiteral("Please login first"));
        return;
    }

    m_signaling->createMeeting(title, password, 16);
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

    m_signaling->joinMeeting(meetingId.trimmed(), password);
    setStatusText(QStringLiteral("Joining meeting..."));
}

void MeetingController::leaveMeeting() {
    if (!m_inMeeting) {
        return;
    }

    resetMeetingState();
    setStatusText(QStringLiteral("Left meeting"));
    emit infoMessage(QStringLiteral("Left meeting"));
}

void MeetingController::toggleAudio() {
    if (!m_inMeeting) {
        return;
    }
    m_audioMuted = !m_audioMuted;
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

void MeetingController::resetMeetingState() {
    if (!m_inMeeting && m_meetingId.isEmpty() && m_participants.isEmpty()) {
        return;
    }

    m_inMeeting = false;
    m_audioMuted = false;
    m_videoMuted = false;
    m_meetingId.clear();
    m_participants.clear();

    emit inMeetingChanged();
    emit audioMutedChanged();
    emit videoMutedChanged();
    emit meetingIdChanged();
    emit participantsChanged();
}
