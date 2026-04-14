#include "RuntimeSmokeDriver.h"

#include "MeetingController.h"
#include "av/render/VideoFrameStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QtGlobal>
#include <algorithm>

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

bool containsSmokePrefixedSegment(const QString& path) {
    const QString normalized = QDir::fromNativeSeparators(path);
    const QStringList segments = normalized.split(QChar('/'), Qt::SkipEmptyParts);
    for (const auto& segment : segments) {
        if (segment.startsWith(QStringLiteral("_smoke_"), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString smokeArtifactBaseDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.trimmed().isEmpty()) {
        base = QDir::tempPath();
    }

    QDir dir(base);
    dir.mkpath(QStringLiteral("meeting-smoke-artifacts"));
    return dir.filePath(QStringLiteral("meeting-smoke-artifacts"));
}

QString normalizeSmokeArtifactPath(const QString& rawPath, const QString& fallbackFileName) {
    const QString trimmed = rawPath.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    const QFileInfo info(trimmed);
    const QString fileName = info.fileName().isEmpty() ? fallbackFileName : info.fileName();
    if (containsSmokePrefixedSegment(trimmed)) {
        return QDir(smokeArtifactBaseDir()).filePath(fileName);
    }

    if (info.isRelative()) {
        const QString normalized = QDir::fromNativeSeparators(trimmed);
        return QDir(smokeArtifactBaseDir()).filePath(normalized);
    }

    return trimmed;
}

bool hasDecodedVideoFrame(av::render::VideoFrameStore* frameStore) {
    if (frameStore == nullptr) {
        return false;
    }

    av::codec::DecodedVideoFrame frame;
    if (!frameStore->snapshot(frame)) {
        return false;
    }

    return frame.width > 0 && frame.height > 0 && !frame.yPlane.empty() && !frame.uvPlane.empty();
}

struct AudioSmokeSnapshot {
    quint64 sentPackets{0};
    quint64 receivedPackets{0};
    quint64 playedFrames{0};
    quint32 rttMs{0};
    quint32 targetBitrateBps{0};
};

AudioSmokeSnapshot collectAudioSmokeSnapshot(const MeetingController* controller) {
    AudioSmokeSnapshot snapshot;
    if (controller == nullptr) {
        return snapshot;
    }

    snapshot.sentPackets = controller->audioSentPacketCount();
    snapshot.receivedPackets = controller->audioReceivedPacketCount();
    snapshot.playedFrames = controller->audioPlayedFrameCount();
    snapshot.rttMs = controller->audioLastRttMs();
    snapshot.targetBitrateBps = controller->audioTargetBitrateBps();
    return snapshot;
}

bool hasAudioEvidence(const AudioSmokeSnapshot& snapshot) {
    constexpr quint32 kMinAudioBitrateBps = 16000U;
    constexpr quint32 kMaxAudioBitrateBps = 64000U;
    return snapshot.sentPackets > 0 &&
           snapshot.receivedPackets > 0 &&
           snapshot.playedFrames > 0 &&
           snapshot.rttMs > 0 &&
           snapshot.targetBitrateBps >= kMinAudioBitrateBps &&
           snapshot.targetBitrateBps <= kMaxAudioBitrateBps;
}

QString formatAudioSmokeSnapshot(const AudioSmokeSnapshot& snapshot, bool evidenceReady) {
    return QStringLiteral("audio_pipeline=sent:%1,recv:%2,played:%3,rtt_ms:%4,target_bps:%5,evidence:%6")
        .arg(snapshot.sentPackets)
        .arg(snapshot.receivedPackets)
        .arg(snapshot.playedFrames)
        .arg(snapshot.rttMs)
        .arg(snapshot.targetBitrateBps)
        .arg(evidenceReady ? 1 : 0);
}

QString normalizeExpectedCameraSource(const QString& raw) {
    const QString normalized = raw.trimmed().toLower();
    if (normalized == QStringLiteral("real") || normalized == QStringLiteral("real-device")) {
        return QStringLiteral("real-device");
    }
    if (normalized == QStringLiteral("synthetic") || normalized == QStringLiteral("synthetic-fallback")) {
        return QStringLiteral("synthetic-fallback");
    }
    return {};
}

bool isDegradableVideoEncoderFailure(const QString& text, bool requireVideoEvidence) {
    if (requireVideoEvidence) {
        return false;
    }
    return text.contains(QStringLiteral("video encoder configure failed"), Qt::CaseInsensitive);
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
    , m_enabled(envFlag("MEETING_RUNTIME_SMOKE"))
    , m_requireVideoEvidence(envFlag("MEETING_SMOKE_REQUIRE_VIDEO"))
    , m_requireAudioEvidence(envFlag("MEETING_SMOKE_REQUIRE_AUDIO"))
    , m_disableLocalVideo(envFlag("MEETING_SMOKE_DISABLE_LOCAL_VIDEO"))
    , m_soakDurationMs(std::max(0, envInt("MEETING_SMOKE_SOAK_MS", 0)))
    , m_soakPollIntervalMs(std::max(200, envInt("MEETING_SMOKE_SOAK_POLL_INTERVAL_MS", 1000)))
    , m_expectedCameraSource([]() {
        const QString configured = normalizeExpectedCameraSource(envValue("MEETING_SMOKE_EXPECT_CAMERA_SOURCE"));
        if (!configured.isEmpty()) {
            return configured;
        }
        if (envFlag("MEETING_SMOKE_EXPECT_REAL_CAMERA")) {
            return QStringLiteral("real-device");
        }
        return QString();
    }()) {
    m_meetingIdPath = normalizeSmokeArtifactPath(m_meetingIdPath, QStringLiteral("meeting_id.txt"));
    m_resultPath = normalizeSmokeArtifactPath(m_resultPath, QStringLiteral("result.txt"));
    m_peerResultPath = normalizeSmokeArtifactPath(m_peerResultPath, QStringLiteral("peer.result.txt"));
}

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

    if (m_requireVideoEvidence) {
        auto* frameStore = qobject_cast<av::render::VideoFrameStore*>(m_controller->remoteVideoFrameSource());
        if (frameStore == nullptr) {
            fail(QStringLiteral("remote video frame source unavailable"));
            return;
        }

        QObject::connect(frameStore, &av::render::VideoFrameStore::frameChanged, this, &RuntimeSmokeDriver::maybeUpdateVideoEvidence);
        maybeUpdateVideoEvidence();
    }

    if (m_requireAudioEvidence) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateAudioEvidence);
    }

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

QString RuntimeSmokeDriver::currentStageTag() const {
    if (!m_startedLogin) {
        return QStringLiteral("bootstrap");
    }

    if (!m_controller || !m_controller->loggedIn()) {
        return QStringLiteral("login");
    }

    if (m_role == QStringLiteral("host")) {
        if (!m_startedCreate) {
            return QStringLiteral("host_create_schedule");
        }
        if (!m_controller->inMeeting()) {
            return QStringLiteral("host_create_meeting");
        }
    } else if (m_role == QStringLiteral("guest")) {
        if (!m_startedJoin) {
            return QStringLiteral("guest_wait_meeting_id");
        }
        if (!m_controller->inMeeting()) {
            return QStringLiteral("guest_join_meeting");
        }
    }

    if (m_requireAudioEvidence && !m_audioEvidenceReady) {
        return QStringLiteral("audio_evidence");
    }

    if (m_requireVideoEvidence && !m_videoEvidenceReady) {
        return QStringLiteral("video_evidence");
    }

    if (!m_expectedCameraSource.isEmpty() && !m_cameraSourceObserved) {
        return QStringLiteral("camera_source_evidence");
    }

    if (m_soakStarted) {
        return QStringLiteral("soak_run");
    }

    if (m_soakDurationMs <= 0 && m_role == QStringLiteral("host") && !m_peerResultPath.isEmpty() && !m_peerSuccessObserved) {
        return QStringLiteral("peer_success_wait");
    }

    return QStringLiteral("post_join_negotiation");
}

QString RuntimeSmokeDriver::videoPipelineSummary() const {
    const bool includePipeline = m_requireVideoEvidence ||
                                 !m_expectedCameraSource.isEmpty() ||
                                 m_videoCameraFrameObserved ||
                                 m_videoPeerConfiguredObserved ||
                                 m_videoEncodedPacketObserved ||
                                 m_videoRtpSentObserved ||
                                 m_videoRtpReceivedObserved ||
                                 m_videoFrameDecodedObserved ||
                                 m_videoEvidenceReady;
    if (!includePipeline) {
        return {};
    }

    return QStringLiteral("video_pipeline=camera_frame:%1,peer:%2,encode:%3,rtp_send:%4,rtp_recv:%5,decode:%6,frame_store:%7")
        .arg(m_videoCameraFrameObserved ? 1 : 0)
        .arg(m_videoPeerConfiguredObserved ? 1 : 0)
        .arg(m_videoEncodedPacketObserved ? 1 : 0)
        .arg(m_videoRtpSentObserved ? 1 : 0)
        .arg(m_videoRtpReceivedObserved ? 1 : 0)
        .arg(m_videoFrameDecodedObserved ? 1 : 0)
        .arg(m_videoEvidenceReady ? 1 : 0);
}

QString RuntimeSmokeDriver::audioPipelineSummary() const {
    const AudioSmokeSnapshot snapshot = collectAudioSmokeSnapshot(m_controller);
    const bool includePipeline = m_requireAudioEvidence ||
                                 m_audioEvidenceReady ||
                                 snapshot.sentPackets > 0 ||
                                 snapshot.receivedPackets > 0 ||
                                 snapshot.playedFrames > 0;
    if (!includePipeline) {
        return {};
    }

    return formatAudioSmokeSnapshot(snapshot, m_audioEvidenceReady);
}

QString RuntimeSmokeDriver::withStageTag(const QString& reason) const {
    const QString normalizedReason = reason.trimmed().isEmpty() ? QStringLiteral("unknown") : reason.trimmed();
    const QString videoPipeline = videoPipelineSummary();
    const QString audioPipeline = audioPipelineSummary();
    QString annotated = QStringLiteral("stage=%1; reason=%2").arg(currentStageTag(), normalizedReason);
    if (!audioPipeline.isEmpty()) {
        annotated += QStringLiteral("; %1").arg(audioPipeline);
    }
    if (!videoPipeline.isEmpty()) {
        annotated += QStringLiteral("; %1").arg(videoPipeline);
    }
    if (!m_videoEncodeDetail.trimmed().isEmpty()) {
        annotated += QStringLiteral("; video_encode_detail=%1").arg(m_videoEncodeDetail.trimmed());
    }
    return annotated;
}

void RuntimeSmokeDriver::handleInfoMessage(const QString& message) {
    if (!m_expectedCameraSource.isEmpty() &&
        message.contains(QStringLiteral("Video camera source: %1").arg(m_expectedCameraSource), Qt::CaseInsensitive)) {
        m_cameraSourceObserved = true;
        if (!m_pendingSuccessReason.isEmpty()) {
            maybeCompleteSuccess(m_pendingSuccessReason);
        }
    }

        if (message.contains(QStringLiteral("Video camera frame observed"), Qt::CaseInsensitive)) {
        m_videoCameraFrameObserved = true;
    }
    if (message.contains(QStringLiteral("Video peer configured"), Qt::CaseInsensitive)) {
        m_videoPeerConfiguredObserved = true;
    }
        if (message.contains(QStringLiteral("Video encoded packet observed"), Qt::CaseInsensitive)) {
        m_videoEncodedPacketObserved = true;
        m_videoEncodeDetail.clear();
    }
    if (message.contains(QStringLiteral("Video encode pending"), Qt::CaseInsensitive)) {
        if (m_videoEncodeDetail.isEmpty()) {
            m_videoEncodeDetail = QStringLiteral("pending");
        }
    }
    if (message.contains(QStringLiteral("Video encode error:"), Qt::CaseInsensitive)) {
        const QString prefix = QStringLiteral("Video encode error:");
        const int index = message.indexOf(prefix, 0, Qt::CaseInsensitive);
        m_videoEncodeDetail = index >= 0 ? message.mid(index + prefix.size()).trimmed() : message.trimmed();
    }
    if (message.contains(QStringLiteral("Video RTP packet sent"), Qt::CaseInsensitive)) {
        m_videoRtpSentObserved = true;
    }
    if (message.contains(QStringLiteral("Video RTP packet received"), Qt::CaseInsensitive)) {
        m_videoRtpReceivedObserved = true;
    }
    if (message.contains(QStringLiteral("Video frame decoded"), Qt::CaseInsensitive)) {
        m_videoFrameDecodedObserved = true;
    }

    if (isDegradableVideoEncoderFailure(message, m_requireVideoEvidence)) {
        writeResult(QStringLiteral("DEGRADED_VIDEO"), withStageTag(message));
        return;
    }

    if (message.contains(QStringLiteral("Failed"), Qt::CaseInsensitive)) {
        fail(message);
        return;
    }

    if (m_role == QStringLiteral("guest") &&
        (message.contains(QStringLiteral("Audio answer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video answer sent"), Qt::CaseInsensitive))) {
        maybeCompleteSuccess(message);
        return;
    }

    if (m_role == QStringLiteral("host") &&
        (message.contains(QStringLiteral("Audio offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Audio endpoint ready"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video endpoint ready"), Qt::CaseInsensitive))) {
        maybeCompleteSuccess(message);
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

    if (m_disableLocalVideo && !m_appliedLocalVideoPolicy) {
        m_appliedLocalVideoPolicy = true;
        if (!m_controller->videoMuted()) {
            m_controller->toggleVideo();
        }
    }

    writeResult(QStringLiteral("IN_MEETING"), m_controller->meetingId());
    if (m_requireAudioEvidence) {
        maybeUpdateAudioEvidence();
    }
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
    if (status.contains(QStringLiteral("expired"), Qt::CaseInsensitive)) {
        fail(status);
        return;
    }

    if (status.contains(QStringLiteral("failed"), Qt::CaseInsensitive)) {
        if (isDegradableVideoEncoderFailure(status, m_requireVideoEvidence)) {
            writeResult(QStringLiteral("DEGRADED_VIDEO"), withStageTag(status));
            return;
        }
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

void RuntimeSmokeDriver::pollPeerSuccess() {
    if (m_reportedResult || m_peerResultPath.isEmpty()) {
        return;
    }

    if (readPeerResult().startsWith(QStringLiteral("SUCCESS"))) {
        m_peerSuccessObserved = true;
        maybeCompleteSuccess(m_pendingSuccessReason.isEmpty() ? QStringLiteral("peer success observed") : m_pendingSuccessReason);
        return;
    }

    QTimer::singleShot(100, this, &RuntimeSmokeDriver::pollPeerSuccess);
}

void RuntimeSmokeDriver::maybeUpdateAudioEvidence() {
    if (m_reportedResult || !m_requireAudioEvidence || m_audioEvidenceReady || !m_controller) {
        return;
    }

    const AudioSmokeSnapshot snapshot = collectAudioSmokeSnapshot(m_controller);
    if (hasAudioEvidence(snapshot)) {
        m_audioEvidenceReady = true;
        maybeCompleteSuccess(m_pendingSuccessReason.isEmpty() ? QStringLiteral("audio evidence observed") : m_pendingSuccessReason);
        return;
    }

    QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateAudioEvidence);
}

void RuntimeSmokeDriver::maybeUpdateVideoEvidence() {
    if (m_reportedResult || !m_requireVideoEvidence || m_videoEvidenceReady || !m_controller) {
        return;
    }

    auto* frameStore = qobject_cast<av::render::VideoFrameStore*>(m_controller->remoteVideoFrameSource());
    if (!hasDecodedVideoFrame(frameStore)) {
        return;
    }

    m_videoEvidenceReady = true;
    maybeCompleteSuccess(m_pendingSuccessReason.isEmpty() ? QStringLiteral("video evidence observed") : m_pendingSuccessReason);
}

void RuntimeSmokeDriver::maybeCompleteSuccess(const QString& reason) {
    if (m_reportedResult) {
        return;
    }

    const QString normalizedReason = reason.trimmed().isEmpty() ? QStringLiteral("success") : reason.trimmed();

    if (m_requireAudioEvidence && !m_audioEvidenceReady) {
        m_pendingSuccessReason = normalizedReason;
        writeResult(QStringLiteral("WAITING_AUDIO"), withStageTag(QStringLiteral("audio evidence pending")));
        return;
    }

    if (m_requireVideoEvidence && !m_videoEvidenceReady) {
        m_pendingSuccessReason = normalizedReason;
        writeResult(QStringLiteral("WAITING_VIDEO"), withStageTag(normalizedReason));
        return;
    }

    if (!m_expectedCameraSource.isEmpty() && !m_cameraSourceObserved) {
        m_pendingSuccessReason = normalizedReason;
        writeResult(QStringLiteral("WAITING_CAMERA_SOURCE"),
                    withStageTag(QStringLiteral("camera source evidence pending: %1").arg(m_expectedCameraSource)));
        return;
    }

    if (m_soakDurationMs > 0) {
        maybeStartSoak(normalizedReason);
        return;
    }

    if (m_role == QStringLiteral("host") && !m_peerResultPath.isEmpty() && !m_peerSuccessObserved) {
        m_pendingSuccessReason = normalizedReason;
        writeResult(QStringLiteral("WAITING_PEER"), withStageTag(normalizedReason));
        if (!m_waitingPeerSuccess) {
            m_waitingPeerSuccess = true;
            QTimer::singleShot(100, this, &RuntimeSmokeDriver::pollPeerSuccess);
        }
        return;
    }

    completeSuccess(normalizedReason);
}

void RuntimeSmokeDriver::maybeStartSoak(const QString& reason) {
    if (m_reportedResult) {
        return;
    }

    if (m_soakDurationMs <= 0) {
        completeSuccess(reason);
        return;
    }

    if (!m_controller || !m_controller->inMeeting()) {
        fail(QStringLiteral("soak interrupted before start"));
        return;
    }

    m_pendingSuccessReason = reason.trimmed().isEmpty() ? QStringLiteral("success") : reason.trimmed();
    if (m_soakStarted) {
        return;
    }

    m_soakStarted = true;
    m_soakTimer.start();
    writeResult(QStringLiteral("SOAKING"), withStageTag(QStringLiteral("soak started: %1ms").arg(m_soakDurationMs)));
    QTimer::singleShot(m_soakPollIntervalMs, this, &RuntimeSmokeDriver::pollSoakProgress);
}

void RuntimeSmokeDriver::pollSoakProgress() {
    if (m_reportedResult || !m_soakStarted) {
        return;
    }

    if (!m_controller || !m_controller->inMeeting()) {
        fail(QStringLiteral("soak interrupted: meeting dropped"));
        return;
    }

    if (m_requireAudioEvidence) {
        maybeUpdateAudioEvidence();
    }

    const qint64 elapsedMs = m_soakTimer.elapsed();
    if (elapsedMs >= m_soakDurationMs) {
        completeSuccess(QStringLiteral("%1; soak_ms=%2")
                            .arg(m_pendingSuccessReason.isEmpty() ? QStringLiteral("success") : m_pendingSuccessReason)
                            .arg(m_soakDurationMs));
        return;
    }

    writeResult(QStringLiteral("SOAKING"),
                withStageTag(QStringLiteral("soak progress: %1/%2ms").arg(elapsedMs).arg(m_soakDurationMs)));
    QTimer::singleShot(m_soakPollIntervalMs, this, &RuntimeSmokeDriver::pollSoakProgress);
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

    const QString annotatedReason = withStageTag(reason);
    m_reportedResult = true;
    writeResult(QStringLiteral("FAIL"), annotatedReason);
    qCritical().noquote() << "[runtime-smoke]" << m_role << "failed:" << annotatedReason;
    QTimer::singleShot(0, qApp, []() {
        QCoreApplication::exit(2);
    });
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

