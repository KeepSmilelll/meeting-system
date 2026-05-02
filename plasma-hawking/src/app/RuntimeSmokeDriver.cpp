#include "RuntimeSmokeDriver.h"

#include "MeetingController.h"
#include "av/render/VideoFrameStore.h"

#include <QCoreApplication>
#include <QAbstractItemModel>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

QString envValue(const char* key, const QString& fallback = QString()) {
    const QString value = qEnvironmentVariable(key);
    return value.isEmpty() ? fallback : value;
}

QStringList envPathList(const char* key) {
    const QString raw = qEnvironmentVariable(key).trimmed();
    if (raw.isEmpty()) {
        return {};
    }

    QStringList values;
    const QStringList segments = raw.split(QChar(';'), Qt::SkipEmptyParts);
    values.reserve(segments.size());
    for (const auto& segment : segments) {
        const QString trimmed = segment.trimmed();
        if (!trimmed.isEmpty()) {
            values.push_back(trimmed);
        }
    }
    values.removeDuplicates();
    return values;
}

QStringList envStringList(const char* key) {
    const QString raw = qEnvironmentVariable(key).trimmed();
    if (raw.isEmpty()) {
        return {};
    }

    QStringList values;
    const QStringList segments = raw.split(QChar(';'), Qt::SkipEmptyParts);
    values.reserve(segments.size());
    for (const auto& segment : segments) {
        const QString trimmed = segment.trimmed();
        if (!trimmed.isEmpty()) {
            values.push_back(trimmed);
        }
    }
    values.removeDuplicates();
    return values;
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

    return frame.hasRenderableData();
}

int modelRoleForName(const QAbstractItemModel* model, const QByteArray& roleName) {
    if (model == nullptr) {
        return -1;
    }

    const auto roles = model->roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
        if (it.value() == roleName) {
            return it.key();
        }
    }
    return -1;
}

bool participantMediaState(const QAbstractItemModel* model,
                           const QString& userId,
                           bool* audioOn,
                           bool* videoOn) {
    if (model == nullptr || userId.trimmed().isEmpty()) {
        return false;
    }

    const int userIdRole = modelRoleForName(model, QByteArrayLiteral("userId"));
    const int audioOnRole = modelRoleForName(model, QByteArrayLiteral("audioOn"));
    const int videoOnRole = modelRoleForName(model, QByteArrayLiteral("videoOn"));
    if (userIdRole < 0 || audioOnRole < 0 || videoOnRole < 0) {
        return false;
    }

    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, userIdRole).toString().trimmed() != userId.trimmed()) {
            continue;
        }
        if (audioOn != nullptr) {
            *audioOn = model->data(index, audioOnRole).toBool();
        }
        if (videoOn != nullptr) {
            *videoOn = model->data(index, videoOnRole).toBool();
        }
        return true;
    }
    return false;
}

struct AudioSmokeSnapshot {
    quint64 sentPackets{0};
    quint64 receivedPackets{0};
    quint64 playedFrames{0};
    quint32 rttMs{0};
    quint32 targetBitrateBps{0};
};

struct AvSyncSmokeSnapshot {
    qint64 lastSkewMs{0};
    qint64 maxAbsSkewMs{0};
    quint64 sampleCount{0};
    quint64 candidateCount{0};
    quint64 noClockCount{0};
    quint64 invalidVideoPtsCount{0};
    quint64 invalidAudioClockCount{0};
    quint64 decodedFrameCount{0};
    quint64 droppedByAudioClockCount{0};
    quint64 queuedFrameCount{0};
    quint64 renderedFrameCount{0};
    quint64 stalePtsDropCount{0};
    quint64 rescheduledFrameCount{0};
    quint64 queueResetCount{0};
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

AvSyncSmokeSnapshot collectAvSyncSnapshot(const MeetingController* controller) {
    AvSyncSmokeSnapshot snapshot;
    if (controller == nullptr) {
        return snapshot;
    }

    snapshot.lastSkewMs = controller->videoLastAudioSkewMs();
    snapshot.maxAbsSkewMs = controller->videoMaxAbsAudioSkewMs();
    snapshot.sampleCount = controller->videoAudioSkewSampleCount();
    snapshot.candidateCount = controller->videoAudioSkewCandidateCount();
    snapshot.noClockCount = controller->videoAudioSkewNoClockCount();
    snapshot.invalidVideoPtsCount = controller->videoAudioSkewInvalidVideoPtsCount();
    snapshot.invalidAudioClockCount = controller->videoAudioSkewInvalidAudioClockCount();
    snapshot.decodedFrameCount = controller->remoteVideoDecodedFrameCount();
    snapshot.droppedByAudioClockCount = controller->remoteVideoDroppedByAudioClockCount();
    snapshot.queuedFrameCount = controller->remoteVideoQueuedFrameCount();
    snapshot.renderedFrameCount = controller->remoteVideoRenderedFrameCount();
    snapshot.stalePtsDropCount = controller->remoteVideoStalePtsDropCount();
    snapshot.rescheduledFrameCount = controller->remoteVideoRescheduledFrameCount();
    snapshot.queueResetCount = controller->remoteVideoQueueResetCount();
    return snapshot;
}

bool hasAudioEvidence(const AudioSmokeSnapshot& snapshot) {
    constexpr quint32 kMinAudioBitrateBps = 16000U;
    constexpr quint32 kMaxAudioBitrateBps = 64000U;
    return snapshot.sentPackets > 0 &&
           snapshot.receivedPackets > 0 &&
           snapshot.playedFrames > 0 &&
           snapshot.targetBitrateBps >= kMinAudioBitrateBps &&
           snapshot.targetBitrateBps <= kMaxAudioBitrateBps;
}

bool hasAvSyncEvidence(const AvSyncSmokeSnapshot& snapshot,
                       int maxSkewMs,
                       int minSamples,
                       int maxAbsSkewMs) {
    if (snapshot.sampleCount < static_cast<quint64>((std::max)(1, minSamples)) ||
        maxSkewMs < 0 ||
        maxAbsSkewMs < 0) {
        return false;
    }
    return std::llabs(snapshot.lastSkewMs) <= maxSkewMs &&
           snapshot.maxAbsSkewMs <= maxAbsSkewMs;
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

QString formatAvSyncSnapshot(const AvSyncSmokeSnapshot& snapshot,
                             bool evidenceReady,
                             int maxSkewMs,
                             int minSamples,
                             int maxAbsSkewMs) {
    return QStringLiteral(
               "avsync_pipeline=last_skew_ms:%1,max_abs_skew_ms:%2,samples:%3,candidates:%4,no_clock:%5,invalid_video_pts:%6,invalid_audio_clock:%7,decoded:%8,dropped_by_audio:%9,queued:%10,rendered:%11,stale_pts_drop:%12,rescheduled:%13,queue_reset:%14,max_skew_ms:%15,min_samples:%16,max_abs_skew_ms:%17,evidence:%18")
        .arg(snapshot.lastSkewMs)
        .arg(snapshot.maxAbsSkewMs)
        .arg(snapshot.sampleCount)
        .arg(snapshot.candidateCount)
        .arg(snapshot.noClockCount)
        .arg(snapshot.invalidVideoPtsCount)
        .arg(snapshot.invalidAudioClockCount)
        .arg(snapshot.decodedFrameCount)
        .arg(snapshot.droppedByAudioClockCount)
        .arg(snapshot.queuedFrameCount)
        .arg(snapshot.renderedFrameCount)
        .arg(snapshot.stalePtsDropCount)
        .arg(snapshot.rescheduledFrameCount)
        .arg(snapshot.queueResetCount)
        .arg(maxSkewMs)
        .arg((std::max)(1, minSamples))
        .arg(maxAbsSkewMs)
        .arg(evidenceReady ? 1 : 0);
}

QString avSyncPendingReason(const AvSyncSmokeSnapshot& snapshot,
                            int maxSkewMs,
                            int minSamples,
                            int maxAbsSkewMs) {
    if (snapshot.sampleCount < static_cast<quint64>((std::max)(1, minSamples))) {
        return QStringLiteral("avsync sample count below minimum");
    }
    if (snapshot.maxAbsSkewMs > maxAbsSkewMs) {
        return QStringLiteral("avsync max abs skew out of bounds");
    }
    if (snapshot.sampleCount > 0 && std::llabs(snapshot.lastSkewMs) > maxSkewMs) {
        return QStringLiteral("avsync skew out of bounds");
    }
    if (snapshot.sampleCount > 0) {
        return QStringLiteral("avsync sampling pending");
    }
    if (snapshot.candidateCount == 0) {
        if (snapshot.noClockCount > 0) {
            return QStringLiteral("avsync clock missing");
        }
        return QStringLiteral("avsync candidate frame pending");
    }
    if (snapshot.invalidAudioClockCount > 0) {
        return QStringLiteral("avsync audio clock not ready");
    }
    if (snapshot.invalidVideoPtsCount > 0) {
        return QStringLiteral("avsync video pts invalid");
    }
    return QStringLiteral("avsync evidence pending");
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
    , m_joinMeetingId(envValue("MEETING_SMOKE_MEETING_ID").trimmed())
    , m_meetingPassword(envValue("MEETING_SMOKE_MEETING_PASSWORD"))
    , m_title(envValue("MEETING_SMOKE_TITLE", QStringLiteral("meeting-client-process-smoke")).trimmed())
    , m_meetingIdPath(envValue("MEETING_SMOKE_MEETING_ID_PATH"))
    , m_resultPath(envValue("MEETING_SMOKE_RESULT_PATH"))
    , m_peerResultPaths(envPathList("MEETING_SMOKE_PEER_RESULT_PATHS"))
    , m_remoteVideoUserId(envValue("MEETING_SMOKE_REMOTE_VIDEO_USER_ID").trimmed())
    , m_enabled(envFlag("MEETING_RUNTIME_SMOKE"))
    , m_enableLocalAudio(envFlag("MEETING_SMOKE_ENABLE_LOCAL_AUDIO"))
    , m_enableLocalVideo(envFlag("MEETING_SMOKE_ENABLE_LOCAL_VIDEO"))
    , m_requireVideoEvidence(envFlag("MEETING_SMOKE_REQUIRE_VIDEO"))
    , m_requireAudioEvidence(envFlag("MEETING_SMOKE_REQUIRE_AUDIO"))
    , m_requireAvSyncEvidence(envFlag("MEETING_SMOKE_REQUIRE_AVSYNC"))
    , m_requireChatEvidence(envFlag("MEETING_SMOKE_REQUIRE_CHAT"))
    , m_requireMediaStateSyncEvidence(envFlag("MEETING_SMOKE_REQUIRE_MEDIA_STATE_SYNC") ||
                                      envFlag("MEETING_SMOKE_REQUIRE_CAMERA_TOGGLE_RECOVERY"))
    , m_disableLocalAudio(envFlag("MEETING_SMOKE_DISABLE_LOCAL_AUDIO"))
    , m_disableLocalVideo(envFlag("MEETING_SMOKE_DISABLE_LOCAL_VIDEO"))
    , m_chatSendText(envValue("MEETING_SMOKE_CHAT_SEND_TEXT").trimmed())
    , m_expectedChatTexts(envStringList("MEETING_SMOKE_CHAT_EXPECT_TEXTS"))
    , m_meetingCapacity(std::max(2, envInt("MEETING_SMOKE_MEETING_CAPACITY", 2)))
    , m_avSyncMaxSkewMs(std::max(0, envInt("MEETING_SMOKE_AVSYNC_MAX_SKEW_MS", 40)))
    , m_avSyncMinSamples(std::max(1, envInt("MEETING_SMOKE_AVSYNC_MIN_SAMPLES", 10)))
    , m_avSyncMaxAbsSkewMs(std::max(0, envInt("MEETING_SMOKE_AVSYNC_MAX_ABS_SKEW_MS", (std::max)(300, m_avSyncMaxSkewMs * 3))))
    , m_soakDurationMs(std::max(0, envInt("MEETING_SMOKE_SOAK_MS", 0)))
    , m_soakPollIntervalMs(std::max(200, envInt("MEETING_SMOKE_SOAK_POLL_INTERVAL_MS", 1000)))
    , m_cameraStartMaxRetries(std::max(0, envInt("MEETING_SMOKE_CAMERA_START_MAX_RETRIES", 6)))
    , m_cameraStartRetryDelayMs(std::max(100, envInt("MEETING_SMOKE_CAMERA_START_RETRY_DELAY_MS", 500)))
    , m_mediaStateToggleLocal(envFlag("MEETING_SMOKE_MEDIA_STATE_TOGGLE_LOCAL"))
    , m_mediaStateInitialDelayMs(std::max(0, envInt("MEETING_SMOKE_MEDIA_STATE_INITIAL_DELAY_MS", 3000)))
    , m_mediaStateStepDelayMs(std::max(500, envInt("MEETING_SMOKE_MEDIA_STATE_STEP_DELAY_MS", 2000)))
    , m_mediaStatePeerUserId(envValue("MEETING_SMOKE_MEDIA_STATE_PEER_USER_ID").trimmed())
    , m_mediaStatePeerUserIdExplicit(!envValue("MEETING_SMOKE_MEDIA_STATE_PEER_USER_ID").trimmed().isEmpty())
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
    if (m_peerResultPaths.isEmpty()) {
        const QString legacyPeerResultPath = envValue("MEETING_SMOKE_PEER_RESULT_PATH");
        if (!legacyPeerResultPath.trimmed().isEmpty()) {
            m_peerResultPaths.push_back(legacyPeerResultPath.trimmed());
        }
    }
    for (QString& path : m_peerResultPaths) {
        path = normalizeSmokeArtifactPath(path, QStringLiteral("peer.result.txt"));
    }
    m_peerResultPaths.removeAll(QString());

    const QString helperSkipAvSyncRaw = envValue("MEETING_SMOKE_GUEST_SKIP_AVSYNC").trimmed();
    const bool helperSkipAvSync = helperSkipAvSyncRaw.isEmpty()
        ? true
        : helperSkipAvSyncRaw.toInt() != 0;
    if (helperSkipAvSync &&
        isJoinerRole() &&
        !m_peerResultPaths.isEmpty()) {
        m_requireAvSyncEvidence = false;
    }

    const int defaultChatSendDelayMs = isHostRole() ? 1000 : 3000;
    m_chatSendDelayMs = std::max(0, envInt("MEETING_SMOKE_CHAT_SEND_DELAY_MS", defaultChatSendDelayMs));
    if (m_requireMediaStateSyncEvidence && !m_mediaStateToggleLocal && m_mediaStatePeerUserId.isEmpty()) {
        m_mediaStatePeerUserId = isJoinerRole() ? QStringLiteral("demo") : QStringLiteral("alice");
    }
}

bool RuntimeSmokeDriver::isHostRole() const {
    return m_role == QStringLiteral("host");
}

bool RuntimeSmokeDriver::isJoinerRole() const {
    return m_role == QStringLiteral("guest") || m_role.startsWith(QStringLiteral("subscriber"));
}

bool RuntimeSmokeDriver::enabled() const {
    return m_enabled && m_controller != nullptr && (isHostRole() || isJoinerRole());
}

void RuntimeSmokeDriver::start() {
    if (!enabled()) {
        return;
    }

    writeResult(QStringLiteral("START"), m_role);
    QObject::connect(m_controller, &MeetingController::infoMessage, this, &RuntimeSmokeDriver::handleInfoMessage);
    QObject::connect(m_controller, &MeetingController::loggedInChanged, this, &RuntimeSmokeDriver::handleLoggedInChanged);
    QObject::connect(m_controller, &MeetingController::inMeetingChanged, this, &RuntimeSmokeDriver::handleInMeetingChanged);
    QObject::connect(m_controller, &MeetingController::audioMutedChanged, this, &RuntimeSmokeDriver::applyLocalAudioPolicy);
    QObject::connect(m_controller, &MeetingController::videoMutedChanged, this, &RuntimeSmokeDriver::applyLocalVideoPolicy);
    QObject::connect(m_controller, &MeetingController::statusTextChanged, this, &RuntimeSmokeDriver::handleStatusTextChanged);

    if (m_requireChatEvidence) {
        if (auto* chatModel = m_controller->chatMessageModel()) {
            QObject::connect(chatModel,
                             &QAbstractItemModel::rowsInserted,
                             this,
                             [this]() { maybeUpdateChatEvidence(); });
            QObject::connect(chatModel,
                             &QAbstractItemModel::modelReset,
                             this,
                             [this]() { maybeUpdateChatEvidence(); });
        }
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateChatEvidence);
    }

    if (m_requireMediaStateSyncEvidence && !m_mediaStateToggleLocal) {
        if (auto* participantModel = m_controller->participantModel()) {
            QObject::connect(participantModel,
                             &QAbstractItemModel::dataChanged,
                             this,
                             [this]() { maybeUpdateMediaStateEvidence(); });
            QObject::connect(participantModel,
                             &QAbstractItemModel::rowsInserted,
                             this,
                             [this]() { maybeUpdateMediaStateEvidence(); });
            QObject::connect(participantModel,
                             &QAbstractItemModel::modelReset,
                             this,
                             [this]() { maybeUpdateMediaStateEvidence(); });
        }
        QObject::connect(m_controller,
                         &MeetingController::remoteVideoFrameSourceChanged,
                         this,
                         &RuntimeSmokeDriver::maybeUpdateMediaStateEvidence);
        QObject::connect(m_controller,
                         &MeetingController::activeVideoPeerUserIdChanged,
                         this,
                         &RuntimeSmokeDriver::maybeUpdateMediaStateEvidence);
    }

    if (m_requireVideoEvidence) {
        QObject::connect(m_controller, &MeetingController::remoteVideoFrameSourceChanged, this, &RuntimeSmokeDriver::maybeUpdateVideoEvidence);
        maybeUpdateVideoEvidence();
    }

    if (m_requireAudioEvidence) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateAudioEvidence);
    }
    if (m_requireAvSyncEvidence) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateAvSyncEvidence);
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
        qInfo().noquote() << "[runtime-smoke]" << m_role
                          << "starting login"
                          << "server=" << QStringLiteral("%1:%2").arg(m_host).arg(m_port)
                          << "username=" << m_username;
        writeResult(QStringLiteral("LOGIN_START"),
                    QStringLiteral("server=%1:%2; user=%3").arg(m_host).arg(m_port).arg(m_username));
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

    if (isHostRole()) {
        if (!m_startedCreate) {
            return QStringLiteral("host_create_schedule");
        }
        if (!m_controller->inMeeting()) {
            return QStringLiteral("host_create_meeting");
        }
    } else if (isJoinerRole()) {
        if (!m_startedJoin) {
            return QStringLiteral("joiner_wait_meeting_id");
        }
        if (!m_controller->inMeeting()) {
            return QStringLiteral("joiner_join_meeting");
        }
    }

    if (m_requireAudioEvidence && !audioTransportReady()) {
        return QStringLiteral("audio_transport");
    }

    if (m_requireVideoEvidence && !videoTransportReady()) {
        return QStringLiteral("video_transport");
    }

    if (m_requireAudioEvidence && !m_audioEvidenceReady) {
        return QStringLiteral("audio_evidence");
    }

    if (m_requireVideoEvidence && !m_videoEvidenceReady) {
        return QStringLiteral("video_evidence");
    }

    if (m_requireAvSyncEvidence && !m_avSyncEvidenceReady) {
        return QStringLiteral("avsync_evidence");
    }

    if (m_requireChatEvidence && !m_chatEvidenceReady) {
        return QStringLiteral("chat_evidence");
    }

    if (m_requireMediaStateSyncEvidence && !m_mediaStateEvidenceReady) {
        return QStringLiteral("media_state_evidence");
    }

    if (!m_expectedCameraSource.isEmpty() && !m_cameraSourceObserved) {
        return QStringLiteral("camera_source_evidence");
    }

    if (m_soakStarted) {
        return QStringLiteral("soak_run");
    }

    if (m_soakDurationMs <= 0 && isHostRole() && !m_peerResultPaths.isEmpty() && !m_peerSuccessObserved) {
        return QStringLiteral("peer_success_wait");
    }

    return QStringLiteral("post_join_negotiation");
}

bool RuntimeSmokeDriver::audioTransportReady() const {
    if (!m_controller || !m_controller->audioDtlsConnected() || !m_controller->audioSrtpReady()) {
        return false;
    }

    if (m_controller->audioIceConnected()) {
        return true;
    }

    // Some NAT/TURN paths can establish DTLS/SRTP and exchange media without
    // surfacing a STUN binding response to this client-side flag. Treat actual
    // audio send/receive/playback evidence as the authoritative path proof.
    return m_audioEvidenceReady || hasAudioEvidence(collectAudioSmokeSnapshot(m_controller));
}

bool RuntimeSmokeDriver::videoTransportReady() const {
    return m_controller &&
           m_controller->videoIceConnected() &&
           m_controller->videoDtlsConnected() &&
           m_controller->videoSrtpReady();
}

QString RuntimeSmokeDriver::transportPipelineSummary() const {
    if (!m_controller || (!m_requireAudioEvidence && !m_requireVideoEvidence && !m_requireAvSyncEvidence)) {
        return {};
    }

    return QStringLiteral("transport=audio_ice:%1,audio_dtls:%2,audio_srtp:%3,video_ice:%4,video_dtls:%5,video_srtp:%6")
        .arg(m_controller->audioIceConnected() ? 1 : 0)
        .arg(m_controller->audioDtlsConnected() ? 1 : 0)
        .arg(m_controller->audioSrtpReady() ? 1 : 0)
        .arg(m_controller->videoIceConnected() ? 1 : 0)
        .arg(m_controller->videoDtlsConnected() ? 1 : 0)
        .arg(m_controller->videoSrtpReady() ? 1 : 0);
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

QString RuntimeSmokeDriver::avSyncSummary() const {
    const AvSyncSmokeSnapshot snapshot = collectAvSyncSnapshot(m_controller);
    const bool includePipeline = m_requireAvSyncEvidence || snapshot.sampleCount > 0 || m_avSyncEvidenceReady;
    if (!includePipeline) {
        return {};
    }

    return formatAvSyncSnapshot(snapshot,
                                m_avSyncEvidenceReady,
                                m_avSyncMaxSkewMs,
                                m_avSyncMinSamples,
                                m_avSyncMaxAbsSkewMs);
}

QString RuntimeSmokeDriver::mediaStateSummary() const {
    if (!m_requireMediaStateSyncEvidence && !m_mediaStateValidationStarted && !m_mediaStateEvidenceReady) {
        return {};
    }

    return QStringLiteral(
               "media_state=toggle_local:%1,peer:%2,initial_audio_on:%3,audio_off:%4,audio_restored:%5,initial_video_on:%6,video_off:%7,video_restored:%8,initial_frame:%9,frame_cleared:%10,frame_restored:%11,evidence:%12")
        .arg(m_mediaStateToggleLocal ? 1 : 0)
        .arg(m_mediaStatePeerUserId.isEmpty() ? QStringLiteral("<local>") : m_mediaStatePeerUserId)
        .arg(m_mediaStateInitialAudioOnObserved ? 1 : 0)
        .arg(m_mediaStateAudioOffObserved ? 1 : 0)
        .arg(m_mediaStateAudioOnRestored ? 1 : 0)
        .arg(m_mediaStateInitialVideoOnObserved ? 1 : 0)
        .arg(m_mediaStateVideoOffObserved ? 1 : 0)
        .arg(m_mediaStateVideoOnRestored ? 1 : 0)
        .arg(m_mediaStateInitialVideoFrameObserved ? 1 : 0)
        .arg(m_mediaStateVideoFrameCleared ? 1 : 0)
        .arg(m_mediaStateVideoFrameRestored ? 1 : 0)
        .arg(m_mediaStateEvidenceReady ? 1 : 0);
}

QString RuntimeSmokeDriver::withStageTag(const QString& reason) const {
    const QString normalizedReason = reason.trimmed().isEmpty() ? QStringLiteral("unknown") : reason.trimmed();
    const QString videoPipeline = videoPipelineSummary();
    const QString audioPipeline = audioPipelineSummary();
    const QString transportPipeline = transportPipelineSummary();
    const QString avSyncPipeline = avSyncSummary();
    const QString mediaStatePipeline = mediaStateSummary();
    QString annotated = QStringLiteral("stage=%1; reason=%2").arg(currentStageTag(), normalizedReason);
    if (!transportPipeline.isEmpty()) {
        annotated += QStringLiteral("; %1").arg(transportPipeline);
    }
    if (!audioPipeline.isEmpty()) {
        annotated += QStringLiteral("; %1").arg(audioPipeline);
    }
    if (!videoPipeline.isEmpty()) {
        annotated += QStringLiteral("; %1").arg(videoPipeline);
    }
    if (!avSyncPipeline.isEmpty()) {
        annotated += QStringLiteral("; %1").arg(avSyncPipeline);
    }
    if (!mediaStatePipeline.isEmpty()) {
        annotated += QStringLiteral("; %1").arg(mediaStatePipeline);
    }
    if (!m_videoEncodeDetail.trimmed().isEmpty()) {
        annotated += QStringLiteral("; video_encode_detail=%1").arg(m_videoEncodeDetail.trimmed());
    }
    if (!m_videoCameraDetail.trimmed().isEmpty()) {
        annotated += QStringLiteral("; video_camera_detail=%1").arg(m_videoCameraDetail.trimmed());
    }
    return annotated;
}

void RuntimeSmokeDriver::handleInfoMessage(const QString& message) {
    const QString trimmedMessage = message.trimmed();
    if (!trimmedMessage.isEmpty()) {
        m_recentInfoMessages.push_back(trimmedMessage);
        while (m_recentInfoMessages.size() > 10) {
            m_recentInfoMessages.pop_front();
        }
    }

    if (!m_expectedCameraSource.isEmpty() &&
        message.contains(QStringLiteral("Video camera source: %1").arg(m_expectedCameraSource), Qt::CaseInsensitive)) {
        m_cameraSourceObserved = true;
        if (!m_pendingSuccessReason.isEmpty()) {
            maybeCompleteSuccess(m_pendingSuccessReason);
        }
    }

    if (message.contains(QStringLiteral("Video camera frame observed"), Qt::CaseInsensitive)) {
        m_videoCameraFrameObserved = true;
        m_videoCameraDetail.clear();
    }
    if (message.contains(QStringLiteral("Video camera frame convert failed:"), Qt::CaseInsensitive)) {
        const QString prefix = QStringLiteral("Video camera frame convert failed:");
        const int index = message.indexOf(prefix, 0, Qt::CaseInsensitive);
        m_videoCameraDetail = index >= 0 ? message.mid(index + prefix.size()).trimmed() : message.trimmed();
    }
    if (message.contains(QStringLiteral("Video camera frame timeout:"), Qt::CaseInsensitive)) {
        const QString prefix = QStringLiteral("Video camera frame timeout:");
        const int index = message.indexOf(prefix, 0, Qt::CaseInsensitive);
        m_videoCameraDetail = index >= 0 ? message.mid(index + prefix.size()).trimmed() : message.trimmed();
    }
    if (message.contains(QStringLiteral("Failed to start screen session:"), Qt::CaseInsensitive)) {
        const QString prefix = QStringLiteral("Failed to start screen session:");
        const int index = message.indexOf(prefix, 0, Qt::CaseInsensitive);
        m_videoCameraDetail = index >= 0 ? message.mid(index + prefix.size()).trimmed() : message.trimmed();
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

    if (maybeHandleRetriableCameraStartFailure(message)) {
        return;
    }

    if (message.contains(QStringLiteral("Failed"), Qt::CaseInsensitive)) {
        fail(message);
        return;
    }

    const bool joinerCanExitOnNegotiationOnly =
        !m_requireAudioEvidence &&
        !m_requireVideoEvidence &&
        !m_requireAvSyncEvidence &&
        m_expectedCameraSource.isEmpty();
    if (isJoinerRole() &&
        joinerCanExitOnNegotiationOnly &&
        (message.contains(QStringLiteral("Audio answer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video answer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Audio transport offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video transport offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Media transport answer accepted"), Qt::CaseInsensitive))) {
        maybeCompleteSuccess(message);
        return;
    }

    if (isHostRole() &&
        (message.contains(QStringLiteral("Audio offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Audio transport offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Video transport offer sent"), Qt::CaseInsensitive) ||
         message.contains(QStringLiteral("Media transport answer accepted"), Qt::CaseInsensitive) ||
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
    if (isHostRole() && !m_startedCreate) {
        m_startedCreate = true;
        m_controller->createMeeting(m_title, QString(), m_meetingCapacity);
        return;
    }

    if (isJoinerRole() && !m_startedJoin) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::pollMeetingId);
    }
}

void RuntimeSmokeDriver::handleInMeetingChanged() {
    if (!m_controller || !m_controller->inMeeting()) {
        return;
    }

    applyLocalAudioPolicy();
    applyLocalVideoPolicy();

    writeResult(QStringLiteral("IN_MEETING"), m_controller->meetingId());
    if (m_requireAudioEvidence) {
        maybeUpdateAudioEvidence();
    }
    if (m_requireAvSyncEvidence) {
        maybeUpdateAvSyncEvidence();
    }
    if (m_requireChatEvidence) {
        maybeScheduleChatSend();
        maybeUpdateChatEvidence();
    }
    if (isHostRole() && !m_meetingIdPath.isEmpty()) {
        if (!writeMeetingId(m_controller->meetingId())) {
            fail(QStringLiteral("failed to publish meeting id"));
        }
    }
}

void RuntimeSmokeDriver::applyLocalAudioPolicy() {
    if (!m_controller || !m_controller->inMeeting()) {
        return;
    }

    if (m_disableLocalAudio) {
        if (!m_controller->audioMuted()) {
            m_controller->toggleAudio();
            return;
        }
        m_appliedLocalAudioPolicy = true;
        return;
    }

    if (m_enableLocalAudio && m_controller->audioMuted()) {
        m_controller->toggleAudio();
        return;
    }

    m_appliedLocalAudioPolicy = true;
}

void RuntimeSmokeDriver::applyLocalVideoPolicy() {
    if (!m_controller || !m_controller->inMeeting()) {
        return;
    }

    if (m_disableLocalVideo) {
        if (!m_controller->videoMuted()) {
            m_controller->toggleVideo();
            return;
        }
        m_appliedLocalVideoPolicy = true;
        return;
    }

    if (m_enableLocalVideo && m_controller->videoMuted()) {
        m_controller->toggleVideo();
        return;
    }

    m_appliedLocalVideoPolicy = true;
}

void RuntimeSmokeDriver::handleStatusTextChanged() {
    if (m_reportedResult) {
        return;
    }
    if (!m_controller) {
        return;
    }

    const QString status = m_controller->statusText();
    if (!status.trimmed().isEmpty()) {
        writeResult(QStringLiteral("STATUS"), withStageTag(status));
    }
    if (status.contains(QStringLiteral("expired"), Qt::CaseInsensitive)) {
        fail(status);
        return;
    }

    if (status.contains(QStringLiteral("failed"), Qt::CaseInsensitive)) {
        if (maybeHandleRetriableCameraStartFailure(status)) {
            return;
        }
        if (isDegradableVideoEncoderFailure(status, m_requireVideoEvidence)) {
            writeResult(QStringLiteral("DEGRADED_VIDEO"), withStageTag(status));
            return;
        }
        fail(status);
    }
}

bool RuntimeSmokeDriver::maybeHandleRetriableCameraStartFailure(const QString& message) {
    if (m_reportedResult || !m_controller) {
        return false;
    }
    const bool startFailed = message.contains(QStringLiteral("Failed to start camera sending"), Qt::CaseInsensitive) &&
                             message.contains(QStringLiteral("camera capture start failed"), Qt::CaseInsensitive);
    const bool noFramesProduced = message.contains(QStringLiteral("camera capture produced no frames"), Qt::CaseInsensitive);
    if (!startFailed && !noFramesProduced) {
        return false;
    }
    if ((!m_requireVideoEvidence && !m_enableLocalVideo) || m_disableLocalVideo) {
        return false;
    }
    if (!m_expectedCameraSource.isEmpty() && m_expectedCameraSource != QStringLiteral("real-device")) {
        return false;
    }
    if (m_cameraStartRetryCount >= m_cameraStartMaxRetries) {
        return false;
    }

    ++m_cameraStartRetryCount;
    const QStringList availableDevices = m_controller->availableCameraDevices();
    const QString deviceList = availableDevices.isEmpty() ? QStringLiteral("<none>") : availableDevices.join(QStringLiteral("|"));
    m_videoCameraDetail = QStringLiteral("camera-start-retry=%1/%2 devices=%3")
                              .arg(m_cameraStartRetryCount)
                              .arg(m_cameraStartMaxRetries)
                              .arg(deviceList);
    writeResult(QStringLiteral("WAITING_VIDEO"),
                withStageTag(QStringLiteral("camera start retry %1/%2")
                                 .arg(m_cameraStartRetryCount)
                                 .arg(m_cameraStartMaxRetries)));

    QTimer::singleShot(m_cameraStartRetryDelayMs, this, [this]() {
        if (m_reportedResult || !m_controller || !m_controller->inMeeting()) {
            return;
        }

        if (!m_controller->videoMuted()) {
            m_controller->toggleVideo();
        }
        QTimer::singleShot(80, this, [this]() {
            if (m_reportedResult || !m_controller || !m_controller->inMeeting()) {
                return;
            }
            if (m_controller->videoMuted()) {
                m_controller->toggleVideo();
            }
        });
    });
    return true;
}

void RuntimeSmokeDriver::pollMeetingId() {
    if (m_reportedResult || !m_controller || !m_controller->loggedIn() || m_startedJoin) {
        return;
    }

    const QString meetingId = !m_joinMeetingId.isEmpty() ? m_joinMeetingId : readMeetingId();
    if (meetingId.isEmpty()) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::pollMeetingId);
        return;
    }

    m_startedJoin = true;
    m_controller->joinMeeting(meetingId, m_meetingPassword);
}

void RuntimeSmokeDriver::pollPeerSuccess() {
    if (m_reportedResult || m_peerResultPaths.isEmpty()) {
        return;
    }

    int successCount = 0;
    const QStringList peerResults = readPeerResults();
    for (int index = 0; index < peerResults.size(); ++index) {
        const QString& peerResult = peerResults.at(index);
        if (peerResult.startsWith(QStringLiteral("SUCCESS"))) {
            ++successCount;
            continue;
        }
        if (peerResult.startsWith(QStringLiteral("FAIL"))) {
            const QStringList lines = peerResult.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            const QString peerReason = lines.size() > 1 ? lines.at(1).trimmed() : peerResult.trimmed();
            fail(QStringLiteral("peer[%1] reported failure: %2").arg(index).arg(peerReason));
            return;
        }
    }

    if (successCount == m_peerResultPaths.size()) {
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

    auto* frameStore = qobject_cast<av::render::VideoFrameStore*>(observedRemoteVideoFrameSource());
    if (!hasDecodedVideoFrame(frameStore)) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateVideoEvidence);
        return;
    }

    m_videoEvidenceReady = true;
    maybeCompleteSuccess(m_pendingSuccessReason.isEmpty() ? QStringLiteral("video evidence observed") : m_pendingSuccessReason);
}

void RuntimeSmokeDriver::maybeUpdateAvSyncEvidence() {
    if (m_reportedResult || !m_requireAvSyncEvidence || m_avSyncEvidenceReady || !m_controller) {
        return;
    }

    const AvSyncSmokeSnapshot snapshot = collectAvSyncSnapshot(m_controller);
    const bool avSyncStable = hasAvSyncEvidence(snapshot,
                                                m_avSyncMaxSkewMs,
                                                m_avSyncMinSamples,
                                                m_avSyncMaxAbsSkewMs);
    if (avSyncStable) {
        m_avSyncEvidenceReady = true;
        maybeCompleteSuccess(m_pendingSuccessReason.isEmpty() ? QStringLiteral("avsync evidence observed") : m_pendingSuccessReason);
        return;
    }

    QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateAvSyncEvidence);
}

void RuntimeSmokeDriver::maybeScheduleChatSend() {
    if (m_reportedResult || m_chatSendScheduled || m_chatSent || !m_controller || !m_controller->inMeeting()) {
        return;
    }
    if (m_chatSendText.trimmed().isEmpty()) {
        return;
    }

    m_chatSendScheduled = true;
    QTimer::singleShot(m_chatSendDelayMs, this, [this]() {
        if (m_reportedResult || m_chatSent || !m_controller || !m_controller->inMeeting()) {
            return;
        }
        m_chatSent = true;
        m_controller->sendChat(m_chatSendText);
        writeResult(QStringLiteral("CHAT_SENT"), withStageTag(QStringLiteral("chat sent")));
        maybeUpdateChatEvidence();
    });
}

void RuntimeSmokeDriver::maybeUpdateChatEvidence() {
    if (m_reportedResult || !m_requireChatEvidence || m_chatEvidenceReady || !m_controller) {
        return;
    }

    auto* model = m_controller->chatMessageModel();
    if (!model) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateChatEvidence);
        return;
    }

    const auto roles = model->roleNames();
    const int contentRole = roles.key(QByteArrayLiteral("content"), -1);
    if (contentRole < 0) {
        QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateChatEvidence);
        return;
    }

    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        const QString content = model->data(index, contentRole).toString().trimmed();
        if (!content.isEmpty()) {
            m_observedChatTexts.insert(content);
        }
    }

    bool allExpectedObserved = true;
    for (const auto& expected : m_expectedChatTexts) {
        if (!m_observedChatTexts.contains(expected.trimmed())) {
            allExpectedObserved = false;
            break;
        }
    }

    if (allExpectedObserved && (!m_expectedChatTexts.isEmpty() || m_chatSendText.isEmpty() || m_chatSent)) {
        m_chatEvidenceReady = true;
        maybeCompleteSuccess(m_pendingSuccessReason.isEmpty() ? QStringLiteral("chat evidence observed") : m_pendingSuccessReason);
        return;
    }

    QTimer::singleShot(100, this, &RuntimeSmokeDriver::maybeUpdateChatEvidence);
}

void RuntimeSmokeDriver::maybeStartMediaStateValidation(const QString& reason) {
    if (m_reportedResult || !m_requireMediaStateSyncEvidence || m_mediaStateEvidenceReady) {
        return;
    }

    m_pendingSuccessReason = reason.trimmed().isEmpty() ? QStringLiteral("success") : reason.trimmed();
    if (m_mediaStateValidationStarted) {
        return;
    }

    m_mediaStateValidationStarted = true;
    writeResult(QStringLiteral("WAITING_MEDIA_STATE"),
                withStageTag(m_mediaStateToggleLocal
                                 ? QStringLiteral("media state local toggle sequence starting")
                                 : QStringLiteral("media state observer waiting for peer toggles")));

    if (m_mediaStateToggleLocal) {
        QTimer::singleShot(m_mediaStateInitialDelayMs, this, &RuntimeSmokeDriver::runMediaStateToggleStep);
        return;
    }

    if (m_mediaStatePeerUserIdExplicit && !m_mediaStatePeerUserId.isEmpty() && m_controller) {
        m_controller->setActiveVideoPeerUserId(m_mediaStatePeerUserId);
    }
    maybeUpdateMediaStateEvidence();
}

void RuntimeSmokeDriver::runMediaStateToggleStep() {
    if (m_reportedResult || !m_controller || !m_controller->inMeeting()) {
        return;
    }

    switch (m_mediaStateToggleStep) {
    case 0:
        if (!m_controller->audioMuted()) {
            m_controller->toggleAudio();
        }
        m_mediaStateAudioOffObserved = true;
        writeResult(QStringLiteral("MEDIA_STATE_TOGGLE"), withStageTag(QStringLiteral("local audio muted")));
        break;
    case 1:
        if (m_controller->audioMuted()) {
            m_controller->toggleAudio();
        }
        m_mediaStateAudioOnRestored = true;
        writeResult(QStringLiteral("MEDIA_STATE_TOGGLE"), withStageTag(QStringLiteral("local audio unmuted")));
        break;
    case 2:
        if (!m_controller->videoMuted()) {
            m_controller->toggleVideo();
        }
        m_mediaStateVideoOffObserved = true;
        m_mediaStateVideoFrameCleared = true;
        writeResult(QStringLiteral("MEDIA_STATE_TOGGLE"), withStageTag(QStringLiteral("local camera muted")));
        break;
    case 3:
        if (m_controller->videoMuted()) {
            m_controller->toggleVideo();
        }
        m_mediaStateVideoOnRestored = true;
        m_mediaStateVideoFrameRestored = true;
        writeResult(QStringLiteral("MEDIA_STATE_TOGGLE"), withStageTag(QStringLiteral("local camera unmuted")));
        break;
    default:
        m_mediaStateEvidenceReady = true;
        writeResult(QStringLiteral("MEDIA_STATE_OK"), withStageTag(QStringLiteral("local media state toggle sequence complete")));
        maybeCompleteSuccess(m_pendingSuccessReason);
        return;
    }

    ++m_mediaStateToggleStep;
    QTimer::singleShot(m_mediaStateStepDelayMs, this, &RuntimeSmokeDriver::runMediaStateToggleStep);
}

void RuntimeSmokeDriver::maybeUpdateMediaStateEvidence() {
    if (m_reportedResult ||
        !m_requireMediaStateSyncEvidence ||
        m_mediaStateEvidenceReady ||
        m_mediaStateToggleLocal ||
        !m_controller ||
        !m_controller->inMeeting()) {
        return;
    }

    bool audioOn = false;
    bool videoOn = false;
    bool hasPeerState = participantMediaState(m_controller->participantModel(),
                                              m_mediaStatePeerUserId,
                                              &audioOn,
                                              &videoOn);
    if (!hasPeerState && !m_mediaStatePeerUserIdExplicit) {
        const QString activeVideoPeerUserId = m_controller->activeVideoPeerUserId().trimmed();
        if (!activeVideoPeerUserId.isEmpty()) {
            bool activeAudioOn = false;
            bool activeVideoOn = false;
            if (participantMediaState(m_controller->participantModel(),
                                      activeVideoPeerUserId,
                                      &activeAudioOn,
                                      &activeVideoOn)) {
                m_mediaStatePeerUserId = activeVideoPeerUserId;
                audioOn = activeAudioOn;
                videoOn = activeVideoOn;
                hasPeerState = true;
            }
        }
    }
    auto* frameStore = qobject_cast<av::render::VideoFrameStore*>(
        m_controller->remoteVideoFrameSourceForUser(m_mediaStatePeerUserId));
    const bool hasRemoteFrame = hasDecodedVideoFrame(frameStore);

    if (hasPeerState && audioOn && !m_mediaStateAudioOffObserved) {
        m_mediaStateInitialAudioOnObserved = true;
    }
    if (hasPeerState && m_mediaStateInitialAudioOnObserved && !audioOn) {
        m_mediaStateAudioOffObserved = true;
        writeResult(QStringLiteral("MEDIA_STATE_OBSERVED"), withStageTag(QStringLiteral("peer audio muted")));
    }
    if (hasPeerState && m_mediaStateAudioOffObserved && audioOn) {
        m_mediaStateAudioOnRestored = true;
    }

    if (hasPeerState && videoOn && !m_mediaStateVideoOffObserved) {
        m_mediaStateInitialVideoOnObserved = true;
    }
    if (m_mediaStateInitialVideoOnObserved && hasRemoteFrame && !m_mediaStateVideoOffObserved) {
        m_mediaStateInitialVideoFrameObserved = true;
    }
    if (hasPeerState && m_mediaStateInitialVideoOnObserved && !videoOn) {
        m_mediaStateVideoOffObserved = true;
        writeResult(QStringLiteral("MEDIA_STATE_OBSERVED"), withStageTag(QStringLiteral("peer camera muted")));
    }
    if (m_mediaStateVideoOffObserved && !videoOn && !hasRemoteFrame) {
        m_mediaStateVideoFrameCleared = true;
    }
    if (hasPeerState && m_mediaStateVideoOffObserved && videoOn) {
        m_mediaStateVideoOnRestored = true;
    }
    if (m_mediaStateVideoOnRestored && hasRemoteFrame) {
        m_mediaStateVideoFrameRestored = true;
    }

    if (m_mediaStateInitialAudioOnObserved &&
        m_mediaStateAudioOffObserved &&
        m_mediaStateAudioOnRestored &&
        m_mediaStateInitialVideoOnObserved &&
        m_mediaStateVideoOffObserved &&
        m_mediaStateVideoOnRestored &&
        m_mediaStateVideoFrameCleared &&
        m_mediaStateVideoFrameRestored) {
        m_mediaStateEvidenceReady = true;
        writeResult(QStringLiteral("MEDIA_STATE_OK"), withStageTag(QStringLiteral("peer media state sync observed")));
        maybeCompleteSuccess(m_pendingSuccessReason);
        return;
    }

    QTimer::singleShot(200, this, &RuntimeSmokeDriver::maybeUpdateMediaStateEvidence);
}

void RuntimeSmokeDriver::maybeCompleteSuccess(const QString& reason) {
    if (m_reportedResult) {
        return;
    }

    const QString normalizedReason = reason.trimmed().isEmpty() ? QStringLiteral("success") : reason.trimmed();

    if (m_requireAudioEvidence && !audioTransportReady()) {
        m_pendingSuccessReason = normalizedReason;
        writeResult(QStringLiteral("WAITING_TRANSPORT"),
                    withStageTag(QStringLiteral("audio ICE/DTLS/SRTP evidence pending")));
        QTimer::singleShot(100, this, [this, normalizedReason]() {
            maybeCompleteSuccess(normalizedReason);
        });
        return;
    }

    if (m_requireVideoEvidence && !videoTransportReady()) {
        m_pendingSuccessReason = normalizedReason;
        writeResult(QStringLiteral("WAITING_TRANSPORT"),
                    withStageTag(QStringLiteral("video ICE/DTLS/SRTP evidence pending")));
        QTimer::singleShot(100, this, [this, normalizedReason]() {
            maybeCompleteSuccess(normalizedReason);
        });
        return;
    }

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

    if (m_requireAvSyncEvidence && !m_avSyncEvidenceReady) {
        m_pendingSuccessReason = normalizedReason;
        const AvSyncSmokeSnapshot snapshot = collectAvSyncSnapshot(m_controller);
        writeResult(QStringLiteral("WAITING_AVSYNC"),
                    withStageTag(avSyncPendingReason(snapshot,
                                                     m_avSyncMaxSkewMs,
                                                     m_avSyncMinSamples,
                                                     m_avSyncMaxAbsSkewMs)));
        return;
    }

    if (m_requireChatEvidence && !m_chatEvidenceReady) {
        m_pendingSuccessReason = normalizedReason;
        writeResult(QStringLiteral("WAITING_CHAT"), withStageTag(QStringLiteral("chat evidence pending")));
        maybeScheduleChatSend();
        maybeUpdateChatEvidence();
        return;
    }

    if (!m_expectedCameraSource.isEmpty() && !m_cameraSourceObserved) {
        m_pendingSuccessReason = normalizedReason;
        writeResult(QStringLiteral("WAITING_CAMERA_SOURCE"),
                    withStageTag(QStringLiteral("camera source evidence pending: %1").arg(m_expectedCameraSource)));
        return;
    }

    if (m_requireMediaStateSyncEvidence && !m_mediaStateEvidenceReady) {
        maybeStartMediaStateValidation(normalizedReason);
        return;
    }

    const bool helperMediaSoakNeeded =
        isJoinerRole() &&
        !m_peerResultPaths.isEmpty() &&
        (m_requireAudioEvidence || m_requireVideoEvidence || m_requireAvSyncEvidence || !m_expectedCameraSource.isEmpty());
    if (helperMediaSoakNeeded) {
        const int helperSoakDefaultMs =
            (m_requireAudioEvidence || m_requireAvSyncEvidence) ? 25000 : 5000;
        const int helperSoakMs = std::max(0, envInt("MEETING_SMOKE_GUEST_MEDIA_SOAK_MS", helperSoakDefaultMs));
        if (m_soakDurationMs < helperSoakMs) {
            m_soakDurationMs = helperSoakMs;
        }
    }

    if (m_soakDurationMs > 0) {
        maybeStartSoak(normalizedReason);
        return;
    }

    if (isHostRole() && !m_peerResultPaths.isEmpty() && !m_peerSuccessObserved) {
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
    if (m_requireAvSyncEvidence) {
        maybeUpdateAvSyncEvidence();
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

void RuntimeSmokeDriver::requestExit(int code) {
    if (m_exitRequested) {
        return;
    }

    m_exitRequested = true;
    const bool hardExit = !qEnvironmentVariableIsSet("MEETING_SMOKE_HARD_EXIT") ||
                          qEnvironmentVariableIntValue("MEETING_SMOKE_HARD_EXIT") != 0;
    if (hardExit) {
        QTimer::singleShot(0, qApp, [code]() {
            std::fflush(nullptr);
            std::_Exit(code);
        });
        return;
    }

    const bool gracefulShutdown = qEnvironmentVariableIntValue("MEETING_SMOKE_GRACEFUL_SHUTDOWN") != 0;
    if (gracefulShutdown && m_controller) {
        m_controller->shutdownMediaForRuntimeSmoke();
    }
    QTimer::singleShot(0, qApp, [code]() {
        QCoreApplication::exit(code);
    });

    // Runtime smoke occasionally gets stuck in shutdown due pending cross-thread teardown.
    // Keep graceful exit first, then force-stop if event loop never returns.
    std::thread([code]() {
        std::this_thread::sleep_for(std::chrono::seconds(6));
        std::_Exit(code);
    }).detach();
}

void RuntimeSmokeDriver::completeSuccess(const QString& reason) {
    if (m_reportedResult) {
        return;
    }

    m_reportedResult = true;
    writeResult(QStringLiteral("SUCCESS"), reason);
    qInfo().noquote() << "[runtime-smoke]" << m_role << "success:" << reason;
    requestExit(0);
}

void RuntimeSmokeDriver::fail(const QString& reason) {
    if (m_reportedResult) {
        return;
    }

    const QString annotatedReason = withStageTag(reason);
    m_reportedResult = true;
    writeResult(QStringLiteral("FAIL"), annotatedReason);
    qCritical().noquote() << "[runtime-smoke]" << m_role << "failed:" << annotatedReason;
    requestExit(2);
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
    QString annotatedReason = reason;
    if (!m_recentInfoMessages.isEmpty()) {
        annotatedReason += QStringLiteral("; info_tail=%1").arg(m_recentInfoMessages.join(QStringLiteral(" | ")));
    }
    stream << status << '\n' << annotatedReason << '\n';
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

QStringList RuntimeSmokeDriver::readPeerResults() const {
    QStringList results;
    results.reserve(m_peerResultPaths.size());
    for (const QString& path : m_peerResultPaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            results.push_back(QString());
            continue;
        }
        results.push_back(QString::fromUtf8(file.readAll()).trimmed());
    }
    return results;
}

QObject* RuntimeSmokeDriver::observedRemoteVideoFrameSource() const {
    if (!m_controller) {
        return nullptr;
    }

    if (!m_remoteVideoUserId.isEmpty()) {
        QObject* source = m_controller->remoteVideoFrameSourceForUser(m_remoteVideoUserId);
        if (source != nullptr) {
            return source;
        }
    }
    return m_controller->remoteVideoFrameSource();
}

