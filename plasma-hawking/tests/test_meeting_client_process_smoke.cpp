#include <iostream>

#include <functional>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QMediaDevices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QDebug>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include "av/codec/VideoEncoder.h"

namespace {

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

bool canConnect(const QString& host, quint16 port) {
    QTcpSocket socket;
    socket.connectToHost(host, port);
    const bool connected = socket.waitForConnected(200);
    if (connected) {
        socket.disconnectFromHost();
    }
    return connected;
}

quint16 reserveTcpPort() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    return server.serverPort();
}

bool useSyntheticAudio() {
    const QByteArray value = qgetenv("MEETING_PROCESS_SMOKE_SYNTHETIC_AUDIO");
    return value.isEmpty() || value != "0";
}

bool useSyntheticCamera() {
    const QByteArray value = qgetenv("MEETING_PROCESS_SMOKE_SYNTHETIC_CAMERA");
    return value.isEmpty() || value != "0";
}

bool hasDefaultAudioDevices() {
    return !QMediaDevices::defaultAudioInput().isNull() &&
           !QMediaDevices::defaultAudioOutput().isNull();
}

bool hasVideoInputDevices() {
    return !QMediaDevices::videoInputs().isEmpty();
}

bool canEncodeVideoForProcessSmoke() {
    av::codec::VideoEncoder encoder;
    return encoder.configure(640, 360, 5, 500 * 1000);
}

bool requireGuestVideoEvidence() {
    const QByteArray configured = qgetenv("MEETING_PROCESS_SMOKE_REQUIRE_VIDEO");
    if (configured == "1") {
        return true;
    }
    if (configured == "0") {
        return false;
    }
    return canEncodeVideoForProcessSmoke();
}
QString readAllOutput(QProcess& process) {
    return QStringLiteral("stdout:\n%1\nstderr:\n%2")
        .arg(QString::fromLocal8Bit(process.readAllStandardOutput()),
             QString::fromLocal8Bit(process.readAllStandardError()));
}

void printFailure(const QString& message) {
    std::cerr << message.toLocal8Bit().constData() << std::endl;
}

void printStageHint(const QString& stage, const QString& module, const QString& reason = QString()) {
    if (reason.trimmed().isEmpty()) {
        printFailure(QStringLiteral("stage=%1 module=%2").arg(stage, module));
        return;
    }
    printFailure(QStringLiteral("stage=%1 module=%2 reason=%3").arg(stage, module, reason.trimmed()));
}

QString readTextFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringLiteral("<missing>");
    }
    return QString::fromUtf8(file.readAll());
}

QString resultStatusLine(const QString& resultText) {
    return resultText.section('\n', 0, 0).trimmed();
}

QString resultReasonLine(const QString& resultText) {
    return resultText.section('\n', 1, 1).trimmed();
}

QString extractStageTag(const QString& reason) {
    const int marker = reason.indexOf(QStringLiteral("stage="));
    if (marker < 0) {
        return {};
    }

    const int begin = marker + static_cast<int>(QStringLiteral("stage=").size());
    int end = reason.indexOf(QChar(';'), begin);
    if (end < 0) {
        end = reason.size();
    }
    return reason.mid(begin, end - begin).trimmed();
}

QString inferSmokeFailureStage(const QString& status, const QString& reason) {
    const QString stageTag = extractStageTag(reason);
    if (!stageTag.isEmpty()) {
        return QStringLiteral("likely-stage=%1").arg(stageTag);
    }

    const QString normalizedStatus = status.trimmed().toUpper();
    const QString normalizedReason = reason.trimmed().toLower();
    if (normalizedStatus == QStringLiteral("WAITING_VIDEO")) {
        return QStringLiteral("likely-stage=video-evidence");
    }
    if (normalizedStatus == QStringLiteral("WAITING_CAMERA_SOURCE")) {
        return QStringLiteral("likely-stage=camera-source-evidence");
    }
    if (normalizedReason.contains(QStringLiteral("login"))) {
        return QStringLiteral("likely-stage=login");
    }
    if (normalizedReason.contains(QStringLiteral("meeting id"))) {
        return QStringLiteral("likely-stage=meeting-id-sync");
    }
    if (normalizedReason.contains(QStringLiteral("offer")) || normalizedReason.contains(QStringLiteral("answer"))) {
        return QStringLiteral("likely-stage=post-join-negotiation");
    }
    if (normalizedReason.contains(QStringLiteral("timeout"))) {
        return QStringLiteral("likely-stage=runtime-timeout");
    }
    return QStringLiteral("likely-stage=unknown");
}

QString inferSmokeFailureBucket(const QString& status, const QString& reason) {
    const QString normalizedStatus = status.trimmed().toUpper();
    const QString normalizedReason = reason.trimmed().toLower();
    if (normalizedStatus == QStringLiteral("WAITING_VIDEO")) {
        return QStringLiteral("likely-module=media-transport-or-decoder");
    }
    if (normalizedStatus == QStringLiteral("WAITING_CAMERA_SOURCE")) {
        return QStringLiteral("likely-module=video-capture-source");
    }
    if (normalizedStatus == QStringLiteral("DEGRADED_VIDEO") || normalizedReason.contains(QStringLiteral("encoder"))) {
        return QStringLiteral("likely-module=video-encoder");
    }
    if (normalizedReason.contains(QStringLiteral("offer")) ||
        normalizedReason.contains(QStringLiteral("answer")) ||
        normalizedReason.contains(QStringLiteral("meeting id")) ||
        normalizedReason.contains(QStringLiteral("login"))) {
        return QStringLiteral("likely-module=signaling-or-negotiation");
    }
    if (normalizedReason.contains(QStringLiteral("timeout"))) {
        return QStringLiteral("likely-module=runtime-timeout");
    }
    if (normalizedStatus == QStringLiteral("FAIL")) {
        return QStringLiteral("likely-module=generic-failure");
    }
    return QStringLiteral("likely-module=unknown");
}

bool waitForSuccessfulExit(QCoreApplication& app, QProcess& process, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        app.processEvents(QEventLoop::AllEvents, 20);
        if (process.state() == QProcess::NotRunning) {
            return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        }
        QThread::msleep(20);
    }
    return false;
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

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        printFailure(QStringLiteral("failed to create temp dir"));
        printStageHint(QStringLiteral("bootstrap_tempdir"), QStringLiteral("runtime-bootstrap"));
        return 1;
    }

    const bool syntheticAudio = useSyntheticAudio();
    const bool syntheticCamera = useSyntheticCamera();
    const bool requireVideoEvidence = requireGuestVideoEvidence();
    if (!syntheticAudio && !hasDefaultAudioDevices()) {
        std::cout << "SKIP no default audio input/output available" << std::endl;
        return 77;
    }

    if (!syntheticCamera && !hasVideoInputDevices()) {
        std::cout << "SKIP no camera device available for real-camera mode" << std::endl;
        return 77;
    }

    const quint16 serverPort = reserveTcpPort();
    if (serverPort == 0) {
        printFailure(QStringLiteral("failed to reserve tcp port"));
        printStageHint(QStringLiteral("bootstrap_port_reserve"), QStringLiteral("runtime-bootstrap"));
        return 1;
    }

    QProcess server;
    QProcessEnvironment serverEnv = QProcessEnvironment::systemEnvironment();
    serverEnv.insert(QStringLiteral("SIGNALING_LISTEN_ADDR"), QStringLiteral("127.0.0.1:%1").arg(serverPort));
    serverEnv.insert(QStringLiteral("SIGNALING_ENABLE_REDIS"), QStringLiteral("false"));
    serverEnv.insert(QStringLiteral("SIGNALING_MYSQL_DSN"), QString());
    server.setProcessEnvironment(serverEnv);
    server.setWorkingDirectory(QStringLiteral("D:/meeting/meeting-server/signaling"));
    const QString goExecutable = resolveGoExecutable();
    server.start(goExecutable, {QStringLiteral("run"), QStringLiteral(".")});
    if (!server.waitForStarted(10000)) {
        const QString startError = QStringLiteral("failed to start signaling server with %1, error=%2")
                               .arg(goExecutable, server.errorString());
        printFailure(startError);
        printStageHint(QStringLiteral("signaling_server_start"), QStringLiteral("signaling-or-negotiation"), startError);
        return 1;
    }

    const auto stopProcess = [](QProcess& process) {
        if (process.state() == QProcess::NotRunning) {
            return;
        }
        process.terminate();
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(5000);
        }
    };

    if (!waitForCondition(app, [serverPort] {
            return canConnect(QStringLiteral("127.0.0.1"), serverPort);
        }, 20000)) {
        const QString listenError = QStringLiteral("server did not listen in time\n%1").arg(readAllOutput(server));
        printFailure(listenError);
        printStageHint(QStringLiteral("signaling_server_listen"), QStringLiteral("signaling-or-negotiation"), listenError);
        stopProcess(server);
        return 1;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString clientExe = QDir(appDir).filePath(QStringLiteral("meeting_client.exe"));
    if (!QFileInfo::exists(clientExe)) {
        const QString clientMissing = QStringLiteral("meeting_client.exe not found at %1").arg(clientExe);
        printFailure(clientMissing);
        printStageHint(QStringLiteral("client_binary_lookup"), QStringLiteral("runtime-bootstrap"), clientMissing);
        stopProcess(server);
        return 1;
    }

    const QString meetingIdPath = tempDir.filePath(QStringLiteral("meeting_id.txt"));
    const QString hostResultPath = tempDir.filePath(QStringLiteral("host.result.txt"));
    const QString guestResultPath = tempDir.filePath(QStringLiteral("guest.result.txt"));

    auto buildClientEnv = [&](const QString& role,
                              const QString& username,
                              const QString& password,
                              const QString& dbName,
                              const QString& resultPath,
                              const QString& peerResultPath) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("MEETING_RUNTIME_SMOKE"), QStringLiteral("1"));
        env.insert(QStringLiteral("MEETING_SYNTHETIC_SCREEN"), QStringLiteral("1"));
        if (syntheticCamera) {
            env.insert(QStringLiteral("MEETING_SYNTHETIC_CAMERA"), QStringLiteral("1"));
            env.insert(QStringLiteral("MEETING_SMOKE_EXPECT_CAMERA_SOURCE"), QStringLiteral("synthetic-fallback"));
            env.remove(QStringLiteral("MEETING_SMOKE_EXPECT_REAL_CAMERA"));
        } else {
            env.remove(QStringLiteral("MEETING_SYNTHETIC_CAMERA"));
            env.insert(QStringLiteral("MEETING_SMOKE_EXPECT_CAMERA_SOURCE"), QStringLiteral("real-device"));
            env.remove(QStringLiteral("MEETING_SMOKE_EXPECT_REAL_CAMERA"));
        }
        if (syntheticAudio) {
            env.insert(QStringLiteral("MEETING_SYNTHETIC_AUDIO"), QStringLiteral("1"));
        } else {
            env.remove(QStringLiteral("MEETING_SYNTHETIC_AUDIO"));
        }
        env.insert(QStringLiteral("MEETING_SMOKE_ROLE"), role);
        env.insert(QStringLiteral("MEETING_SMOKE_USERNAME"), username);
        env.insert(QStringLiteral("MEETING_SMOKE_PASSWORD"), password);
        env.insert(QStringLiteral("MEETING_SERVER_HOST"), QStringLiteral("127.0.0.1"));
        env.insert(QStringLiteral("MEETING_SERVER_PORT"), QString::number(serverPort));
        env.insert(QStringLiteral("MEETING_DB_PATH"), tempDir.filePath(dbName));
        env.insert(QStringLiteral("MEETING_SMOKE_MEETING_ID_PATH"), meetingIdPath);
        env.insert(QStringLiteral("MEETING_SMOKE_RESULT_PATH"), resultPath);
        env.insert(QStringLiteral("MEETING_SMOKE_PEER_RESULT_PATH"), peerResultPath);
        env.insert(QStringLiteral("MEETING_SMOKE_TIMEOUT_MS"), QStringLiteral("30000"));
        if (requireVideoEvidence && role == QStringLiteral("guest")) {
            env.insert(QStringLiteral("MEETING_SMOKE_REQUIRE_VIDEO"), QStringLiteral("1"));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_REQUIRE_VIDEO"));
        }
        return env;
    };

    QProcess host;
    host.setProcessEnvironment(buildClientEnv(QStringLiteral("host"), QStringLiteral("demo"), QStringLiteral("demo"), QStringLiteral("host.sqlite"), hostResultPath, guestResultPath));
    host.setWorkingDirectory(appDir);
    host.start(clientExe, {});
    if (!host.waitForStarted(10000)) {
        printFailure(QStringLiteral("failed to start host client"));
        printStageHint(QStringLiteral("host_process_start"), QStringLiteral("runtime-bootstrap"));
        stopProcess(server);
        return 1;
    }

    QProcess guest;
    guest.setProcessEnvironment(buildClientEnv(QStringLiteral("guest"), QStringLiteral("alice"), QStringLiteral("alice"), QStringLiteral("guest.sqlite"), guestResultPath, hostResultPath));
    guest.setWorkingDirectory(appDir);
    guest.start(clientExe, {});
    if (!guest.waitForStarted(10000)) {
        printFailure(QStringLiteral("failed to start guest client"));
        printStageHint(QStringLiteral("guest_process_start"), QStringLiteral("runtime-bootstrap"));
        stopProcess(host);
        stopProcess(server);
        return 1;
    }

    const bool hostOk = waitForSuccessfulExit(app, host, 45000);
    const bool guestOk = waitForSuccessfulExit(app, guest, 45000);

    if (!hostOk || !guestOk) {
        const QString hostResult = readTextFile(hostResultPath);
        const QString guestResult = readTextFile(guestResultPath);
        printFailure(QStringLiteral("process smoke failed"));
        printFailure(QStringLiteral("video evidence required=%1 encoder available=%2")
                         .arg(requireVideoEvidence ? 1 : 0)
                         .arg(canEncodeVideoForProcessSmoke() ? 1 : 0));
        printFailure(QStringLiteral("host result status=%1 reason=%2")
                         .arg(resultStatusLine(hostResult), resultReasonLine(hostResult)));
        printFailure(QStringLiteral("guest result status=%1 reason=%2")
                         .arg(resultStatusLine(guestResult), resultReasonLine(guestResult)));
        printFailure(QStringLiteral("host %1")
                         .arg(inferSmokeFailureBucket(resultStatusLine(hostResult), resultReasonLine(hostResult))));
        printFailure(QStringLiteral("guest %1")
                         .arg(inferSmokeFailureBucket(resultStatusLine(guestResult), resultReasonLine(guestResult))));
        printFailure(QStringLiteral("host-stage %1")
                         .arg(inferSmokeFailureStage(resultStatusLine(hostResult), resultReasonLine(hostResult))));
        printFailure(QStringLiteral("guest-stage %1")
                         .arg(inferSmokeFailureStage(resultStatusLine(guestResult), resultReasonLine(guestResult))));
        printFailure(QStringLiteral("host exitStatus=%1 exitCode=%2 result=\n%3")
                         .arg(host.exitStatus())
                         .arg(host.exitCode())
                         .arg(hostResult));
        printFailure(QStringLiteral("guest exitStatus=%1 exitCode=%2 result=\n%3")
                         .arg(guest.exitStatus())
                         .arg(guest.exitCode())
                         .arg(guestResult));
        printFailure(QStringLiteral("host:\n%1").arg(readAllOutput(host)));
        printFailure(QStringLiteral("guest:\n%1").arg(readAllOutput(guest)));
        printFailure(QStringLiteral("server:\n%1").arg(readAllOutput(server)));
        stopProcess(host);
        stopProcess(guest);
        stopProcess(server);
        return 1;
    }

    stopProcess(server);
    std::cout << "test_meeting_client_process_smoke passed" << std::endl;
    return 0;
}
