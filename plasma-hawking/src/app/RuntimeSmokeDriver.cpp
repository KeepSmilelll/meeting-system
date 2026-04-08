#include "RuntimeSmokeDriver.h"

#include "MeetingController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>
#include <QtGlobal>

namespace {

QString envValue(const char* key, const QString& fallback = QString()) {
    const QString value = qEnvironmentVariable(key);
    return value.isEmpty() ? fallback : value;
}

bool envFlag(const char* key) {
    return qEnvironmentVariableIntValue(key) != 0;
}

int envInt(const char* key, int fallback) {
    bool ok = false;
    const int value = qEnvironmentVariable(key).toInt(&ok);
    return ok ? value : fallback;
}

}  // namespace

RuntimeSmokeDriver::RuntimeSmokeDriver(MeetingController* controller, QObject* parent)
    : QObject(parent)
    , m_controller(controller)
    , m_role(envValue("MEETING_SMOKE_ROLE").trimmed().toLower())
    , m_host(envValue("MEETING_SERVER_HOST", QStringLiteral("127.0.0.1")).trimmed())
    , m_port(static_cast<quint16>(envInt("MEETING_SERVER_PORT", 8443)))
    , m_username(envValue("MEETING_SMOKE_USERNAME").trimmed())
    , m_password(envValue("MEETING_SMOKE_PASSWORD"))
    , m_title(envValue("MEETING_SMOKE_TITLE", QStringLiteral("meeting-client-process-smoke")).trimmed())
    , m_meetingIdPath(envValue("MEETING_SMOKE_MEETING_ID_PATH"))
    , m_resultPath(envValue("MEETING_SMOKE_RESULT_PATH"))
    , m_peerResultPath(envValue("MEETING_SMOKE_PEER_RESULT_PATH"))
    , m_enabled(envFlag("MEETING_RUNTIME_SMOKE")) {}

bool RuntimeSmokeDriver::enabled() const {
    return m_enabled && m_controller != nullptr && (m_role == QStringLiteral("host") || m_role == QStringLiteral("guest"));
}

void RuntimeSmokeDriver::start() {
    if (!enabled()) {
        return;
    }

    writeResult(QStringLiteral("START"), m_role);
    QObject::connect(m_controller, &MeetingController::infoMessage, this, &RuntimeSmokeDriver::handleInfoMessage);
    QObject::connect(m_controller, &MeetingController::loggedInChanged, this, &RuntimeSmokeDriver::handleLoggedInChanged);
    QObject::connect(m_controller, &MeetingController::inMeetingChanged, this, &RuntimeSmokeDriver::handleInMeetingChanged);
    QObject::connect(m_controller, &MeetingController::statusTextChanged, this, &RuntimeSmokeDriver::handleStatusTextChanged);

    m_controller->setServerEndpoint(m_host, m_port);

    const int timeoutMs = envInt("MEETING_SMOKE_TIMEOUT_MS", 30000);
    QTimer::singleShot(timeoutMs, this, [this]() {
        fail(QStringLiteral("timeout"));
    });

    QTimer::singleShot(0, this, [this]() {
        if (m_startedLogin) {
            return;
        }
        m_startedLogin = true;
        m_controller->login(m_username, m_password);
    });
}

void RuntimeSmokeDriver::handleInfoMessage(const QString& message) {
    if (message.contains(QStringLiteral("Failed"), Qt::CaseInsensitive)) {
        fail(message);
        return;
    }

    if (m_role == QStringLiteral("guest") &&
        (message.contains(QStringLiteral("Audio answer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video answer sent"), Qt::CaseInsensitive))) {
        completeSuccess(message);
        return;
    }

    if (m_role == QStringLiteral("host") &&
        (message.contains(QStringLiteral("Audio offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Audio endpoint ready"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video endpoint ready"), Qt::CaseInsensitive))) {
        if (m_peerResultPath.isEmpty()) {
            completeSuccess(message);
            return;
        }

        m_pendingSuccessReason = message;
        writeResult(QStringLiteral("WAITING_PEER"), message);
        if (!m_waitingPeerSuccess) {
            m_waitingPeerSuccess = true;
            QTimer::singleShot(100, this, &RuntimeSmokeDriver::pollPeerSuccess);
        }
    }
}

void RuntimeSmokeDriver::handleLoggedInChanged() {
    if (!m_controller || !m_controller->loggedIn()) {
        return;
    }

    writeResult(QStringLiteral("LOGGED_IN"), m_role);
    if (m_role == QStringLiteral("host") && !m_startedCreate) {
        m_startedCreate = true;
        m_controller->createMeeting(m_title, QString(), 2);
        return;
    }

    if (m_role == QStringLiteral("guest") && !m_startedJoin) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::pollMeetingId);
    }
}

void RuntimeSmokeDriver::handleInMeetingChanged() {
    if (!m_controller || !m_controller->inMeeting()) {
        return;
    }

    writeResult(QStringLiteral("IN_MEETING"), m_controller->meetingId());
    if (m_role == QStringLiteral("host") && !m_meetingIdPath.isEmpty()) {
        if (!writeMeetingId(m_controller->meetingId())) {
            fail(QStringLiteral("failed to publish meeting id"));
        }
    }
}

void RuntimeSmokeDriver::handleStatusTextChanged() {
    if (!m_controller) {
        return;
    }

    const QString status = m_controller->statusText();
    if (status.contains(QStringLiteral("failed"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("expired"), Qt::CaseInsensitive)) {
        fail(status);
    }
}

void RuntimeSmokeDriver::pollMeetingId() {
    if (m_reportedResult || !m_controller || !m_controller->loggedIn() || m_startedJoin) {
        return;
    }

    const QString meetingId = readMeetingId();
    if (meetingId.isEmpty()) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::pollMeetingId);
        return;
    }

    m_startedJoin = true;
    m_controller->joinMeeting(meetingId, QString());
}

void RuntimeSmokeDriver::completeSuccess(const QString& reason) {
    if (m_reportedResult) {
        return;
    }

    m_reportedResult = true;
    writeResult(QStringLiteral("SUCCESS"), reason);
    qInfo().noquote() << "[runtime-smoke]" << m_role << "success:" << reason;
    QTimer::singleShot(0, qApp, []() {
        QCoreApplication::exit(0);
    });
}

void RuntimeSmokeDriver::fail(const QString& reason) {
    if (m_reportedResult) {
        return;
    }

    m_reportedResult = true;
    writeResult(QStringLiteral("FAIL"), reason);
    qCritical().noquote() << "[runtime-smoke]" << m_role << "failed:" << reason;
    QTimer::singleShot(0, qApp, []() {
        QCoreApplication::exit(2);
    });
}

void RuntimeSmokeDriver::pollPeerSuccess() {
    if (m_reportedResult || m_peerResultPath.isEmpty()) {
        return;
    }

    if (readPeerResult().startsWith(QStringLiteral("SUCCESS"))) {
        completeSuccess(m_pendingSuccessReason.isEmpty() ? QStringLiteral("peer success observed") : m_pendingSuccessReason);
        return;
    }

    QTimer::singleShot(100, this, &RuntimeSmokeDriver::pollPeerSuccess);
}

bool RuntimeSmokeDriver::writeMeetingId(const QString& meetingId) {
    if (m_meetingIdPath.isEmpty() || meetingId.isEmpty()) {
        return false;
    }

    const QFileInfo info(m_meetingIdPath);
    if (!info.dir().exists() && !info.dir().mkpath(QStringLiteral("."))) {
        return false;
    }

    QFile file(m_meetingIdPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << meetingId << '\n';
    return stream.status() == QTextStream::Ok;
}

void RuntimeSmokeDriver::writeResult(const QString& status, const QString& reason) const {
    if (m_resultPath.isEmpty()) {
        return;
    }

    const QFileInfo info(m_resultPath);
    if (!info.dir().exists()) {
        info.dir().mkpath(QStringLiteral("."));
    }

    QFile file(m_resultPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << status << '\n' << reason << '\n';
}

QString RuntimeSmokeDriver::readMeetingId() const {
    if (m_meetingIdPath.isEmpty()) {
        return {};
    }

    QFile file(m_meetingIdPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    return stream.readLine().trimmed();
}

QString RuntimeSmokeDriver::readPeerResult() const {
    if (m_peerResultPath.isEmpty()) {
        return {};
    }

    QFile file(m_peerResultPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    return QString::fromUtf8(file.readAll()).trimmed();
}
