#include <functional>
#include <cstdlib>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHostAddress>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QMediaDevices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QStringList>
#include <QDebug>

#include "app/MeetingController.h"
#include "av/codec/VideoEncoder.h"
#include "av/render/VideoFrameStore.h"

namespace {


struct ControllerProbe {
    QStringList infoMessages;
};

struct VideoSmokeDiagnostics {
    bool hostVideoSignalReady{false};
    bool guestVideoSignalReady{false};
    bool hostVideoOfferSent{false};
    bool guestVideoAnswerSent{false};
    bool hostVideoDegraded{false};
    bool guestVideoDegraded{false};
    bool hostDecodedFrameReady{false};
    bool guestDecodedFrameReady{false};
};

struct AudioSmokeDiagnostics {
    quint64 hostSentPackets{0};
    quint64 hostReceivedPackets{0};
    quint64 hostPlayedFrames{0};
    quint64 guestSentPackets{0};
    quint64 guestReceivedPackets{0};
    quint64 guestPlayedFrames{0};
    quint32 hostRttMs{0};
    quint32 guestRttMs{0};
    quint32 hostTargetBitrateBps{0};
    quint32 guestTargetBitrateBps{0};
};

struct AvSyncSmokeDiagnostics {
    qint64 hostLastSkewMs{0};
    qint64 guestLastSkewMs{0};
    qint64 hostMaxAbsSkewMs{0};
    qint64 guestMaxAbsSkewMs{0};
    quint64 hostSampleCount{0};
    quint64 guestSampleCount{0};
};

struct SignalingStageDiagnostics {
    bool loginReady{false};
    bool hostCreateReady{false};
    bool guestJoinReady{false};
};

bool waitForCondition(QCoreApplication& app, const std::function<bool()>& condition, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (condition()) {
            return true;
        }
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return condition();
}

quint16 reserveLocalPort() {
    QTcpServer socket;
    if (!socket.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    return socket.serverPort();
}

bool canConnectToServer(quint16 serverPort) {
    if (serverPort == 0) {
        return false;
    }

    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), serverPort);
    const bool connected = socket.waitForConnected(200);
    if (connected) {
        socket.disconnectFromHost();
    }
    return connected;
}

bool canEncodeVideoForRuntimeSmoke() {
    av::codec::VideoEncoder encoder;
    return encoder.configure(640, 360, 5, 500 * 1000);
}

bool requireAvSyncEvidenceForRuntimeSmoke() {
    return qEnvironmentVariableIntValue("MEETING_RUNTIME_SMOKE_REQUIRE_AVSYNC") != 0;
}

qint64 maxAvSyncSkewMsForRuntimeSmoke() {
    bool ok = false;
    const qint64 configured = qEnvironmentVariable("MEETING_RUNTIME_SMOKE_MAX_AVSYNC_SKEW_MS").toLongLong(&ok);
    if (ok && configured >= 0) {
        return configured;
    }
    return 40;
}

bool useSyntheticAudioForRuntimeSmoke() {
    const QByteArray value = qgetenv("MEETING_RUNTIME_SMOKE_SYNTHETIC_AUDIO");
    return value.isEmpty() || value != "0";
}

bool hasDefaultAudioDevices() {
    return !QMediaDevices::defaultAudioInput().isNull() &&
           !QMediaDevices::defaultAudioOutput().isNull();
}
bool useSyntheticCameraForRuntimeSmoke() {
    const QByteArray value = qgetenv("MEETING_RUNTIME_SMOKE_SYNTHETIC_CAMERA");
    return value.isEmpty() || value != "0";
}

bool hasVideoInputDevices() {
    return !QMediaDevices::videoInputs().isEmpty();
}

QString collectedOutput(QProcess& process) {
    const QString stdoutText = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError());
    return QStringLiteral("stdout:\n%1\nstderr:\n%2").arg(stdoutText, stderrText);
}

bool containsMessage(const QStringList& messages, const QString& needle) {
    for (const QString& message : messages) {
        if (message.contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool hasVideoNegotiationEvidence(const QStringList& messages) {
    return containsMessage(messages, QStringLiteral("Video offer sent")) ||
           containsMessage(messages, QStringLiteral("Video answer sent")) ||
           containsMessage(messages, QStringLiteral("Video endpoint ready"));
}

bool hasCameraSourceEvidence(const QStringList& messages, bool syntheticFallback) {
    return containsMessage(messages, syntheticFallback
                                       ? QStringLiteral("Video camera source: synthetic-fallback")
                                       : QStringLiteral("Video camera source: real-device"));
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

AudioSmokeDiagnostics collectAudioSmokeDiagnostics(const MeetingController& hostController,
                                                   const MeetingController& guestController) {
    AudioSmokeDiagnostics diagnostics;
    diagnostics.hostSentPackets = hostController.audioSentPacketCount();
    diagnostics.hostReceivedPackets = hostController.audioReceivedPacketCount();
    diagnostics.hostPlayedFrames = hostController.audioPlayedFrameCount();
    diagnostics.guestSentPackets = guestController.audioSentPacketCount();
    diagnostics.guestReceivedPackets = guestController.audioReceivedPacketCount();
    diagnostics.guestPlayedFrames = guestController.audioPlayedFrameCount();
    diagnostics.hostRttMs = hostController.audioLastRttMs();
    diagnostics.guestRttMs = guestController.audioLastRttMs();
    diagnostics.hostTargetBitrateBps = hostController.audioTargetBitrateBps();
    diagnostics.guestTargetBitrateBps = guestController.audioTargetBitrateBps();
    return diagnostics;
}

bool hasDualEndAudioEvidence(const AudioSmokeDiagnostics& diagnostics) {
    return diagnostics.hostSentPackets > 0 &&
           diagnostics.hostReceivedPackets > 0 &&
           diagnostics.hostPlayedFrames > 0 &&
           diagnostics.guestSentPackets > 0 &&
           diagnostics.guestReceivedPackets > 0 &&
           diagnostics.guestPlayedFrames > 0;
}

bool hasAudioRtcpEvidence(const AudioSmokeDiagnostics& diagnostics) {
    constexpr quint32 kMinAudioBitrateBps = 16000U;
    constexpr quint32 kMaxAudioBitrateBps = 64000U;
    const bool hostBitrateInRange = diagnostics.hostTargetBitrateBps >= kMinAudioBitrateBps &&
                                    diagnostics.hostTargetBitrateBps <= kMaxAudioBitrateBps;
    const bool guestBitrateInRange = diagnostics.guestTargetBitrateBps >= kMinAudioBitrateBps &&
                                     diagnostics.guestTargetBitrateBps <= kMaxAudioBitrateBps;
    return diagnostics.hostRttMs > 0 &&
           diagnostics.guestRttMs > 0 &&
           hostBitrateInRange &&
           guestBitrateInRange;
}

QString formatAudioSmokeDiagnostics(const AudioSmokeDiagnostics& diagnostics) {
    return QStringLiteral("audio-diagnostics: host_sent=%1 host_recv=%2 host_played=%3 host_rtt_ms=%4 host_target_bps=%5 guest_sent=%6 guest_recv=%7 guest_played=%8 guest_rtt_ms=%9 guest_target_bps=%10")
        .arg(diagnostics.hostSentPackets)
        .arg(diagnostics.hostReceivedPackets)
        .arg(diagnostics.hostPlayedFrames)
        .arg(diagnostics.hostRttMs)
        .arg(diagnostics.hostTargetBitrateBps)
        .arg(diagnostics.guestSentPackets)
        .arg(diagnostics.guestReceivedPackets)
        .arg(diagnostics.guestPlayedFrames)
        .arg(diagnostics.guestRttMs)
        .arg(diagnostics.guestTargetBitrateBps);
}

QString inferAudioFailureBucket(const AudioSmokeDiagnostics& diagnostics) {
    const bool hostSendMissing = diagnostics.hostSentPackets == 0;
    const bool guestSendMissing = diagnostics.guestSentPackets == 0;
    if (hostSendMissing || guestSendMissing) {
        return QStringLiteral("likely-module=audio-capture-or-encoder");
    }

    const bool hostReceiveMissing = diagnostics.hostReceivedPackets == 0 || diagnostics.hostPlayedFrames == 0;
    const bool guestReceiveMissing = diagnostics.guestReceivedPackets == 0 || diagnostics.guestPlayedFrames == 0;
    if (hostReceiveMissing || guestReceiveMissing) {
        return QStringLiteral("likely-module=audio-transport-or-decoder");
    }

    constexpr quint32 kMinAudioBitrateBps = 16000U;
    constexpr quint32 kMaxAudioBitrateBps = 64000U;
    const bool hostRtcpMissing = diagnostics.hostRttMs == 0 ||
                                 diagnostics.hostTargetBitrateBps < kMinAudioBitrateBps ||
                                 diagnostics.hostTargetBitrateBps > kMaxAudioBitrateBps;
    const bool guestRtcpMissing = diagnostics.guestRttMs == 0 ||
                                  diagnostics.guestTargetBitrateBps < kMinAudioBitrateBps ||
                                  diagnostics.guestTargetBitrateBps > kMaxAudioBitrateBps;
    if (hostRtcpMissing || guestRtcpMissing) {
        return QStringLiteral("likely-module=rtcp-rtt-or-audio-bwe");
    }
    return QStringLiteral("likely-module=audio-unknown");
}

AvSyncSmokeDiagnostics collectAvSyncSmokeDiagnostics(const MeetingController& hostController,
                                                     const MeetingController& guestController) {
    AvSyncSmokeDiagnostics diagnostics;
    diagnostics.hostLastSkewMs = hostController.videoLastAudioSkewMs();
    diagnostics.guestLastSkewMs = guestController.videoLastAudioSkewMs();
    diagnostics.hostMaxAbsSkewMs = hostController.videoMaxAbsAudioSkewMs();
    diagnostics.guestMaxAbsSkewMs = guestController.videoMaxAbsAudioSkewMs();
    diagnostics.hostSampleCount = hostController.videoAudioSkewSampleCount();
    diagnostics.guestSampleCount = guestController.videoAudioSkewSampleCount();
    return diagnostics;
}

bool hasDualEndAvSyncEvidence(const AvSyncSmokeDiagnostics& diagnostics, qint64 maxSkewMs) {
    return diagnostics.hostSampleCount > 0 &&
           diagnostics.guestSampleCount > 0 &&
           std::llabs(diagnostics.hostLastSkewMs) <= maxSkewMs &&
           std::llabs(diagnostics.guestLastSkewMs) <= maxSkewMs;
}

QString formatAvSyncSmokeDiagnostics(const AvSyncSmokeDiagnostics& diagnostics, qint64 maxSkewMs) {
    return QStringLiteral("avsync-diagnostics: host_last_skew_ms=%1 host_max_abs_skew_ms=%2 host_samples=%3 guest_last_skew_ms=%4 guest_max_abs_skew_ms=%5 guest_samples=%6 max_skew_ms=%7")
        .arg(diagnostics.hostLastSkewMs)
        .arg(diagnostics.hostMaxAbsSkewMs)
        .arg(diagnostics.hostSampleCount)
        .arg(diagnostics.guestLastSkewMs)
        .arg(diagnostics.guestMaxAbsSkewMs)
        .arg(diagnostics.guestSampleCount)
        .arg(maxSkewMs);
}

QString inferAvSyncFailureBucket(const AvSyncSmokeDiagnostics& diagnostics, qint64 maxSkewMs) {
    if (diagnostics.hostSampleCount == 0 || diagnostics.guestSampleCount == 0) {
        return QStringLiteral("likely-module=avsync-sampler");
    }
    if (std::llabs(diagnostics.hostLastSkewMs) > maxSkewMs ||
        std::llabs(diagnostics.guestLastSkewMs) > maxSkewMs) {
        return QStringLiteral("likely-module=avsync-audio-clock");
    }
    return QStringLiteral("likely-module=avsync-unknown");
}

VideoSmokeDiagnostics collectVideoSmokeDiagnostics(const ControllerProbe& hostProbe,
                                                   const ControllerProbe& guestProbe,
                                                   av::render::VideoFrameStore* hostRemoteVideoFrameStore,
                                                   av::render::VideoFrameStore* guestRemoteVideoFrameStore) {
    VideoSmokeDiagnostics diagnostics;
    diagnostics.hostVideoSignalReady = hasVideoNegotiationEvidence(hostProbe.infoMessages);
    diagnostics.guestVideoSignalReady = hasVideoNegotiationEvidence(guestProbe.infoMessages);
    diagnostics.hostVideoOfferSent = containsMessage(hostProbe.infoMessages, QStringLiteral("Video offer sent"));
    diagnostics.guestVideoAnswerSent = containsMessage(guestProbe.infoMessages, QStringLiteral("Video answer sent"));
    diagnostics.hostVideoDegraded = containsMessage(hostProbe.infoMessages,
                                                    QStringLiteral("Video encoder unavailable"));
    diagnostics.guestVideoDegraded = containsMessage(guestProbe.infoMessages,
                                                     QStringLiteral("Video encoder unavailable"));
    diagnostics.hostDecodedFrameReady = hasDecodedVideoFrame(hostRemoteVideoFrameStore);
    diagnostics.guestDecodedFrameReady = hasDecodedVideoFrame(guestRemoteVideoFrameStore);
    return diagnostics;
}

QString formatVideoSmokeDiagnostics(const VideoSmokeDiagnostics& diagnostics) {
    return QStringLiteral("video-diagnostics: host_signal=%1 guest_signal=%2 host_offer=%3 guest_answer=%4 host_degraded=%5 guest_degraded=%6 host_decoded=%7 guest_decoded=%8")
        .arg(diagnostics.hostVideoSignalReady ? 1 : 0)
        .arg(diagnostics.guestVideoSignalReady ? 1 : 0)
        .arg(diagnostics.hostVideoOfferSent ? 1 : 0)
        .arg(diagnostics.guestVideoAnswerSent ? 1 : 0)
        .arg(diagnostics.hostVideoDegraded ? 1 : 0)
        .arg(diagnostics.guestVideoDegraded ? 1 : 0)
        .arg(diagnostics.hostDecodedFrameReady ? 1 : 0)
        .arg(diagnostics.guestDecodedFrameReady ? 1 : 0);
}

QString inferVideoFailureBucket(const VideoSmokeDiagnostics& diagnostics) {
    if (diagnostics.hostVideoDegraded || diagnostics.guestVideoDegraded) {
        return QStringLiteral("likely-module=video-encoder");
    }
    if (!diagnostics.hostVideoSignalReady || !diagnostics.guestVideoSignalReady) {
        return QStringLiteral("likely-module=signaling-or-negotiation");
    }
    if (!diagnostics.hostDecodedFrameReady || !diagnostics.guestDecodedFrameReady) {
        return QStringLiteral("likely-module=media-transport-or-decoder");
    }
    return QStringLiteral("likely-module=unknown");
}

QString formatSignalingStageDiagnostics(const SignalingStageDiagnostics& diagnostics) {
    return QStringLiteral("signaling-stage: login=%1 host_create=%2 guest_join=%3")
        .arg(diagnostics.loginReady ? 1 : 0)
        .arg(diagnostics.hostCreateReady ? 1 : 0)
        .arg(diagnostics.guestJoinReady ? 1 : 0);
}

QString inferSignalingFailureStage(const SignalingStageDiagnostics& diagnostics) {
    if (!diagnostics.loginReady) {
        return QStringLiteral("likely-stage=login");
    }
    if (!diagnostics.hostCreateReady) {
        return QStringLiteral("likely-stage=create-meeting");
    }
    if (!diagnostics.guestJoinReady) {
        return QStringLiteral("likely-stage=join-meeting");
    }
    return QStringLiteral("likely-stage=post-join-negotiation");
}

QString resolveGoExecutable() {
    const QString found = QStandardPaths::findExecutable(QStringLiteral("go"));
    if (!found.isEmpty()) {
        return found;
    }

    const QString fallback = QStringLiteral("D:/go-env/go/bin/go.exe");
    if (QFileInfo::exists(fallback)) {
        return fallback;
    }

    return QStringLiteral("go");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const bool syntheticAudio = useSyntheticAudioForRuntimeSmoke();
    if (syntheticAudio) {
        qputenv("MEETING_SYNTHETIC_AUDIO", "1");
    } else {
        qunsetenv("MEETING_SYNTHETIC_AUDIO");
    }
    qputenv("MEETING_SYNTHETIC_SCREEN", "1");
    const bool syntheticCamera = useSyntheticCameraForRuntimeSmoke();
    if (syntheticCamera) {
        qputenv("MEETING_SYNTHETIC_CAMERA", "1");
    } else {
        qunsetenv("MEETING_SYNTHETIC_CAMERA");
    }

    if (!syntheticAudio && !hasDefaultAudioDevices()) {
        qInfo().noquote() << "SKIP no default audio input/output available for real-audio mode";
        return 77;
    }

    if (!syntheticCamera && !hasVideoInputDevices()) {
        qInfo().noquote() << "SKIP no camera device available for real-camera mode";
        return 77;
    }

    const quint16 serverPort = reserveLocalPort();
    if (serverPort == 0) {
        qCritical().noquote() << "failed to reserve signaling listen port";
        qCritical().noquote() << "likely-stage=bootstrap_port_reserve";
        qCritical().noquote() << "likely-module=runtime-bootstrap";
        return 1;
    }
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        qCritical().noquote() << "failed to create temporary directory";
        qCritical().noquote() << "likely-stage=bootstrap_tempdir";
        qCritical().noquote() << "likely-module=runtime-bootstrap";
        return 1;
    }

    QProcess server;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SIGNALING_LISTEN_ADDR"), QStringLiteral("127.0.0.1:%1").arg(serverPort));
    env.insert(QStringLiteral("SIGNALING_ENABLE_REDIS"), QStringLiteral("false"));
    env.insert(QStringLiteral("SIGNALING_MYSQL_DSN"), QString());
    server.setProcessEnvironment(env);
    server.setWorkingDirectory(QStringLiteral("D:/meeting/meeting-server/signaling"));
    const QString goExecutable = resolveGoExecutable();
    server.start(goExecutable, {QStringLiteral("run"), QStringLiteral(".")});
    if (!server.waitForStarted(10000)) {
        qCritical().noquote() << "failed to start signaling server with" << goExecutable
                              << "error:" << server.errorString();
        qCritical().noquote() << "likely-stage=signaling_server_start";
        qCritical().noquote() << "likely-module=signaling-or-negotiation";
        return 1;
    }

    const auto stopServer = [&server]() {
        server.terminate();
        if (!server.waitForFinished(5000)) {
            server.kill();
            server.waitForFinished(5000);
        }
    };

    if (!waitForCondition(app, [serverPort] { return canConnectToServer(serverPort); }, 20000)) {
        qCritical().noquote() << "signaling server did not start listening\n" << collectedOutput(server);
        qCritical().noquote() << "likely-stage=signaling_server_listen";
        qCritical().noquote() << "likely-module=signaling-or-negotiation";
        stopServer();
        return 1;
    }

    const QString hostDbPath = tempDir.filePath(QStringLiteral("host/settings.sqlite"));
    const QString guestDbPath = tempDir.filePath(QStringLiteral("guest/settings.sqlite"));
    MeetingController hostController(hostDbPath);
    MeetingController guestController(guestDbPath);

    ControllerProbe hostProbe;
    ControllerProbe guestProbe;
    QObject::connect(&hostController, &MeetingController::infoMessage, &app, [&hostProbe](const QString& message) {
        hostProbe.infoMessages.push_back(message);
    });
    QObject::connect(&guestController, &MeetingController::infoMessage, &app, [&guestProbe](const QString& message) {
        guestProbe.infoMessages.push_back(message);
    });

    auto* hostRemoteVideoFrameStore = qobject_cast<av::render::VideoFrameStore*>(hostController.remoteVideoFrameSource());
    auto* guestRemoteVideoFrameStore = qobject_cast<av::render::VideoFrameStore*>(guestController.remoteVideoFrameSource());
    if (hostRemoteVideoFrameStore == nullptr || guestRemoteVideoFrameStore == nullptr) {
        qCritical().noquote() << "runtime smoke failed to acquire remote video frame stores";
        qCritical().noquote() << "likely-stage=client_frame_source_init";
        qCritical().noquote() << "likely-module=media-transport-or-decoder";
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    hostController.setServerEndpoint(QStringLiteral("127.0.0.1"), serverPort);
    guestController.setServerEndpoint(QStringLiteral("127.0.0.1"), serverPort);
    SignalingStageDiagnostics signalingStageDiagnostics;

    hostController.login(QStringLiteral("demo"), QStringLiteral("demo"));
    guestController.login(QStringLiteral("alice"), QStringLiteral("alice"));

    if (!waitForCondition(app, [&hostController, &guestController] {
            return hostController.loggedIn() && guestController.loggedIn();
        }, 10000)) {
        qCritical().noquote() << "login smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << "host status:" << hostController.statusText();
        qCritical().noquote() << "guest status:" << guestController.statusText();
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }
    signalingStageDiagnostics.loginReady = true;

    hostController.createMeeting(QStringLiteral("runtime-smoke"), QString(), 2);
    if (!waitForCondition(app, [&hostController] {
            return hostController.inMeeting() && !hostController.meetingId().isEmpty();
        }, 10000)) {
        qCritical().noquote() << "host createMeeting smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << "host status:" << hostController.statusText();
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }
    signalingStageDiagnostics.hostCreateReady = true;

    guestController.joinMeeting(hostController.meetingId(), QString());
    if (!waitForCondition(app, [&hostController, &guestController] {
            return guestController.inMeeting() &&
                   guestController.meetingId() == hostController.meetingId() &&
                   hostController.participants().size() >= 2 &&
                   guestController.participants().size() >= 2;
        }, 10000)) {
        qCritical().noquote() << "guest joinMeeting smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << "host status:" << hostController.statusText();
        qCritical().noquote() << "guest status:" << guestController.statusText();
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }
    signalingStageDiagnostics.guestJoinReady = true;

    if (!waitForCondition(app, [&hostProbe, &guestProbe] {
            return (containsMessage(hostProbe.infoMessages, QStringLiteral("Audio offer sent")) ||
                    containsMessage(hostProbe.infoMessages, QStringLiteral("Video offer sent"))) &&
                   (containsMessage(guestProbe.infoMessages, QStringLiteral("Audio answer sent")) ||
                    containsMessage(guestProbe.infoMessages, QStringLiteral("Video answer sent")));
        }, 15000)) {
        const VideoSmokeDiagnostics diagnostics =
            collectVideoSmokeDiagnostics(hostProbe,
                                         guestProbe,
                                         hostRemoteVideoFrameStore,
                                         guestRemoteVideoFrameStore);
        qCritical().noquote() << "media negotiation smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << formatVideoSmokeDiagnostics(diagnostics);
        qCritical().noquote() << inferVideoFailureBucket(diagnostics);
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    if (!waitForCondition(app, [&hostController, &guestController] {
            const AudioSmokeDiagnostics diagnostics =
                collectAudioSmokeDiagnostics(hostController, guestController);
            return hasDualEndAudioEvidence(diagnostics);
        }, 20000)) {
        const VideoSmokeDiagnostics videoDiagnostics =
            collectVideoSmokeDiagnostics(hostProbe,
                                         guestProbe,
                                         hostRemoteVideoFrameStore,
                                         guestRemoteVideoFrameStore);
        const AudioSmokeDiagnostics audioDiagnostics =
            collectAudioSmokeDiagnostics(hostController, guestController);
        qCritical().noquote() << "dual-end audio smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << formatAudioSmokeDiagnostics(audioDiagnostics);
        qCritical().noquote() << inferAudioFailureBucket(audioDiagnostics);
        qCritical().noquote() << formatVideoSmokeDiagnostics(videoDiagnostics);
        qCritical().noquote() << inferVideoFailureBucket(videoDiagnostics);
        qCritical().noquote() << "host status:" << hostController.statusText();
        qCritical().noquote() << "guest status:" << guestController.statusText();
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    if (!waitForCondition(app, [&hostController, &guestController] {
            const AudioSmokeDiagnostics diagnostics =
                collectAudioSmokeDiagnostics(hostController, guestController);
            return hasAudioRtcpEvidence(diagnostics);
        }, 15000)) {
        const VideoSmokeDiagnostics videoDiagnostics =
            collectVideoSmokeDiagnostics(hostProbe,
                                         guestProbe,
                                         hostRemoteVideoFrameStore,
                                         guestRemoteVideoFrameStore);
        const AudioSmokeDiagnostics audioDiagnostics =
            collectAudioSmokeDiagnostics(hostController, guestController);
        qCritical().noquote() << "audio RTCP RTT smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << formatAudioSmokeDiagnostics(audioDiagnostics);
        qCritical().noquote() << inferAudioFailureBucket(audioDiagnostics);
        qCritical().noquote() << formatVideoSmokeDiagnostics(videoDiagnostics);
        qCritical().noquote() << inferVideoFailureBucket(videoDiagnostics);
        qCritical().noquote() << "host status:" << hostController.statusText();
        qCritical().noquote() << "guest status:" << guestController.statusText();
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }
    const QString expectedCameraSource = syntheticCamera
        ? QStringLiteral("synthetic-fallback")
        : QStringLiteral("real-device");
    if (!waitForCondition(app, [&hostProbe, &guestProbe, syntheticCamera] {
            return hasCameraSourceEvidence(hostProbe.infoMessages, syntheticCamera) ||
                   hasCameraSourceEvidence(guestProbe.infoMessages, syntheticCamera);
        }, 10000)) {
        qCritical().noquote() << "camera source evidence missing";
        qCritical().noquote() << "expected camera source:" << expectedCameraSource;
        qCritical().noquote() << "likely-stage=camera-source-evidence";
        qCritical().noquote() << "likely-module=video-capture-source";
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    const bool requireVideoDecodeEvidence = canEncodeVideoForRuntimeSmoke();
    if (requireVideoDecodeEvidence &&
        !waitForCondition(app, [&hostProbe, &guestProbe] {
            return hasVideoNegotiationEvidence(hostProbe.infoMessages) &&
                   hasVideoNegotiationEvidence(guestProbe.infoMessages);
        }, 10000)) {
        const VideoSmokeDiagnostics diagnostics =
            collectVideoSmokeDiagnostics(hostProbe,
                                         guestProbe,
                                         hostRemoteVideoFrameStore,
                                         guestRemoteVideoFrameStore);
        qCritical().noquote() << "strict video negotiation smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << formatVideoSmokeDiagnostics(diagnostics);
        qCritical().noquote() << inferVideoFailureBucket(diagnostics);
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    if (requireVideoDecodeEvidence &&
        !waitForCondition(app, [hostRemoteVideoFrameStore, guestRemoteVideoFrameStore] {
            return hasDecodedVideoFrame(hostRemoteVideoFrameStore) &&
                   hasDecodedVideoFrame(guestRemoteVideoFrameStore);
        }, 20000)) {
        const VideoSmokeDiagnostics diagnostics =
            collectVideoSmokeDiagnostics(hostProbe,
                                         guestProbe,
                                         hostRemoteVideoFrameStore,
                                         guestRemoteVideoFrameStore);
        qCritical().noquote() << "dual-end remote video decode smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << formatVideoSmokeDiagnostics(diagnostics);
        qCritical().noquote() << inferVideoFailureBucket(diagnostics);
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    const bool requireAvSyncEvidence = requireAvSyncEvidenceForRuntimeSmoke();
    const qint64 maxAvSyncSkewMs = maxAvSyncSkewMsForRuntimeSmoke();
    if (requireVideoDecodeEvidence &&
        requireAvSyncEvidence &&
        !waitForCondition(app, [&hostController, &guestController, maxAvSyncSkewMs] {
            const AvSyncSmokeDiagnostics diagnostics =
                collectAvSyncSmokeDiagnostics(hostController, guestController);
            return hasDualEndAvSyncEvidence(diagnostics, maxAvSyncSkewMs);
        }, 15000)) {
        const VideoSmokeDiagnostics videoDiagnostics =
            collectVideoSmokeDiagnostics(hostProbe,
                                         guestProbe,
                                         hostRemoteVideoFrameStore,
                                         guestRemoteVideoFrameStore);
        const AvSyncSmokeDiagnostics avSyncDiagnostics =
            collectAvSyncSmokeDiagnostics(hostController, guestController);
        qCritical().noquote() << "dual-end AVSync smoke failed";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << formatVideoSmokeDiagnostics(videoDiagnostics);
        qCritical().noquote() << inferVideoFailureBucket(videoDiagnostics);
        qCritical().noquote() << formatAvSyncSmokeDiagnostics(avSyncDiagnostics, maxAvSyncSkewMs);
        qCritical().noquote() << inferAvSyncFailureBucket(avSyncDiagnostics, maxAvSyncSkewMs);
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    if (!requireVideoDecodeEvidence) {
        qInfo().noquote() << "runtime smoke skipped strict video decode assertion (encoder unavailable)";
    }

    if (requireVideoDecodeEvidence &&
        (containsMessage(hostProbe.infoMessages, QStringLiteral("Video encoder unavailable")) ||
         containsMessage(guestProbe.infoMessages, QStringLiteral("Video encoder unavailable")))) {
        const VideoSmokeDiagnostics diagnostics =
            collectVideoSmokeDiagnostics(hostProbe,
                                         guestProbe,
                                         hostRemoteVideoFrameStore,
                                         guestRemoteVideoFrameStore);
        qCritical().noquote() << "strict video smoke detected unexpected encoder downgrade";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << formatVideoSmokeDiagnostics(diagnostics);
        qCritical().noquote() << inferVideoFailureBucket(diagnostics);
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    if (containsMessage(hostProbe.infoMessages, QStringLiteral("Failed")) ||
        containsMessage(guestProbe.infoMessages, QStringLiteral("Failed"))) {
        const VideoSmokeDiagnostics diagnostics =
            collectVideoSmokeDiagnostics(hostProbe,
                                         guestProbe,
                                         hostRemoteVideoFrameStore,
                                         guestRemoteVideoFrameStore);
        qCritical().noquote() << "runtime smoke observed failure messages";
        qCritical().noquote() << formatSignalingStageDiagnostics(signalingStageDiagnostics);
        qCritical().noquote() << inferSignalingFailureStage(signalingStageDiagnostics);
        qCritical().noquote() << formatVideoSmokeDiagnostics(diagnostics);
        qCritical().noquote() << inferVideoFailureBucket(diagnostics);
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    guestController.leaveMeeting();
    hostController.leaveMeeting();
    waitForCondition(app, [&hostController, &guestController] {
        return !hostController.inMeeting() && !guestController.inMeeting();
    }, 5000);

    stopServer();
    qInfo().noquote() << "test_meeting_runtime_smoke passed";
    return 0;
}
