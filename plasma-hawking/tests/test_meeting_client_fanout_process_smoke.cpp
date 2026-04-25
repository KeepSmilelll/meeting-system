#include <array>
#include <functional>
#include <iostream>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "av/codec/VideoEncoder.h"

namespace {

int finishProcessSmoke(QTemporaryDir& tempDir, int exitCode) {
    std::cout.flush();
    std::cerr.flush();
    const bool keepTemp = qEnvironmentVariableIntValue("MEETING_PROCESS_SMOKE_KEEP_TEMP") != 0;
    if (keepTemp) {
        std::cout << "fanout_temp_dir=" << tempDir.path().toLocal8Bit().constData() << std::endl;
    } else {
        tempDir.remove();
    }
#if defined(Q_OS_WIN)
    ::ExitProcess(static_cast<UINT>(exitCode));
#endif
    return exitCode;
}

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

bool waitForSuccessfulExit(QCoreApplication& app, QProcess& process, int timeoutMs) {
    return waitForCondition(app,
                            [&process]() {
                                return process.state() == QProcess::NotRunning;
                            },
                            timeoutMs) &&
           process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
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

quint16 reserveUdpPort() {
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    return socket.localPort();
}

bool useSyntheticCamera() {
    const QByteArray value = qgetenv("MEETING_PROCESS_SMOKE_SYNTHETIC_CAMERA");
    return value.isEmpty() || value != "0";
}

int envIntValue(const char* key, int fallback) {
    bool ok = false;
    const int value = qEnvironmentVariable(key).toInt(&ok);
    return ok ? value : fallback;
}

bool requireVideoEvidence() {
    const QByteArray configured = qgetenv("MEETING_PROCESS_SMOKE_REQUIRE_VIDEO");
    if (configured == "1") {
        return true;
    }
    if (configured == "0") {
        return false;
    }

    av::codec::VideoEncoder encoder;
    return encoder.configure(640, 360, 5, 500 * 1000);
}

QString readAllOutput(QProcess& process) {
    return QStringLiteral("stdout:\n%1\nstderr:\n%2")
        .arg(QString::fromLocal8Bit(process.readAllStandardOutput()),
             QString::fromLocal8Bit(process.readAllStandardError()));
}

void printFailure(const QString& message) {
    std::cerr << message.toLocal8Bit().constData() << std::endl;
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

bool hasPublishedMeetingId(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    return !QString::fromUtf8(file.readAll()).trimmed().isEmpty();
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
    if (normalizedStatus == QStringLiteral("WAITING_TRANSPORT")) {
        return QStringLiteral("likely-stage=ice-dtls-srtp");
    }
    if (normalizedStatus == QStringLiteral("WAITING_PEER")) {
        return QStringLiteral("likely-stage=peer-success-wait");
    }
    if (normalizedStatus == QStringLiteral("WAITING_CAMERA_SOURCE")) {
        return QStringLiteral("likely-stage=camera-source-evidence");
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
    if (normalizedStatus == QStringLiteral("WAITING_TRANSPORT")) {
        return QStringLiteral("likely-module=ice-dtls-srtp");
    }
    if (normalizedStatus == QStringLiteral("WAITING_CAMERA_SOURCE")) {
        return QStringLiteral("likely-module=video-capture-source");
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
    return QStringLiteral("likely-module=unknown");
}

QString resolveGoExecutable() {
    const QString found = QStandardPaths::findExecutable(QStringLiteral("go"));
    if (!found.isEmpty()) {
        return found;
    }

    const QString fallback = QStringLiteral("D:\\go-env\\go\\bin\\go.exe");
    if (QFileInfo::exists(fallback)) {
        return fallback;
    }

    return QStringLiteral("go");
}

QString resolveInternalSfuExecutable() {
    const QString configured = qEnvironmentVariable("MEETING_PROCESS_SMOKE_SFU_EXE").trimmed();
    const QStringList candidates = {
        configured,
        QStringLiteral("D:/meeting/meeting-server/sfu/build_sfu_check/Debug/meeting_sfu.exe"),
        QStringLiteral("D:/meeting/meeting-server/sfu/build/Debug/meeting_sfu.exe"),
        QStringLiteral("D:/meeting/meeting-server/sfu/build_sfu_check/Release/meeting_sfu.exe"),
        QStringLiteral("D:/meeting/meeting-server/sfu/build/Release/meeting_sfu.exe"),
    };

    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

#if defined(Q_OS_WIN)
std::vector<DWORD> childProcessIds(DWORD parentPid) {
    std::vector<DWORD> children;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return children;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parentPid) {
                children.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return children;
}

void terminateProcessTree(DWORD rootPid) {
    if (rootPid == 0 || rootPid == GetCurrentProcessId()) {
        return;
    }

    for (const DWORD childPid : childProcessIds(rootPid)) {
        terminateProcessTree(childPid);
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, rootPid);
    if (process == nullptr) {
        return;
    }

    TerminateProcess(process, 1);
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
}
#endif

struct ClientRuntime {
    ClientRuntime(QString roleValue,
                  QString usernameValue,
                  QString passwordValue,
                  QString dbNameValue,
                  QString resultPathValue,
                  QString stdoutPathValue,
                  QString stderrPathValue,
                  QString observedRemoteVideoUserIdValue,
                  QString cameraDeviceValue,
                  bool enableLocalVideoValue,
                  bool disableLocalVideoValue,
                  bool requireVideoEvidenceValue,
                  bool expectCameraSourceValue)
        : role(std::move(roleValue))
        , username(std::move(usernameValue))
        , password(std::move(passwordValue))
        , dbName(std::move(dbNameValue))
        , resultPath(std::move(resultPathValue))
        , stdoutPath(std::move(stdoutPathValue))
        , stderrPath(std::move(stderrPathValue))
        , observedRemoteVideoUserId(std::move(observedRemoteVideoUserIdValue))
        , cameraDevice(std::move(cameraDeviceValue))
        , enableLocalVideo(enableLocalVideoValue)
        , disableLocalVideo(disableLocalVideoValue)
        , requireVideoEvidence(requireVideoEvidenceValue)
        , expectCameraSource(expectCameraSourceValue)
        , process(nullptr) {}

    QString role;
    QString username;
    QString password;
    QString dbName;
    QString resultPath;
    QString stdoutPath;
    QString stderrPath;
    QString observedRemoteVideoUserId;
    QString cameraDevice;
    bool enableLocalVideo{false};
    bool disableLocalVideo{false};
    bool requireVideoEvidence{false};
    bool expectCameraSource{false};
    QProcess process;
    qint64 rootPid{0};
};

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        printFailure(QStringLiteral("failed to create temp dir"));
        return 1;
    }

    const bool syntheticCamera = useSyntheticCamera();
    const bool needVideoEvidence = requireVideoEvidence();
    const int timeoutMs = (std::max)(10000, envIntValue("MEETING_PROCESS_SMOKE_TIMEOUT_MS", 90000));
    const int runtimeTimeoutMs = (std::max)(30000, envIntValue("MEETING_PROCESS_SMOKE_RUNTIME_TIMEOUT_MS", 60000));
    const int soakMs = (std::max)(0, envIntValue("MEETING_PROCESS_SMOKE_SOAK_MS", 0));
    const QString hostCameraDevice = qEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_CAMERA_DEVICE").trimmed();

    const QString configuredSignalingHost = qEnvironmentVariable("MEETING_PROCESS_SMOKE_SIGNALING_HOST").trimmed();
    const int configuredSignalingPort = envIntValue("MEETING_PROCESS_SMOKE_SIGNALING_PORT", 0);
    const quint16 externalSignalingPort =
        configuredSignalingPort > 0 && configuredSignalingPort <= 65535
            ? static_cast<quint16>(configuredSignalingPort)
            : 0;
    const bool useExternalSignaling = externalSignalingPort != 0;
    const QString signalingHost = configuredSignalingHost.isEmpty() ? QStringLiteral("127.0.0.1") : configuredSignalingHost;

    quint16 serverPort = externalSignalingPort;
    if (!useExternalSignaling) {
        serverPort = reserveTcpPort();
        if (serverPort == 0) {
            printFailure(QStringLiteral("failed to reserve signaling tcp port"));
            return finishProcessSmoke(tempDir, 1);
        }
    }

    QProcess sfu;
    qint64 sfuRootPid = 0;
    bool internalSfuStarted = false;
    quint16 sfuRpcPort = 0;
    quint16 sfuMediaPort = 0;
    QProcess server;
    qint64 serverRootPid = 0;
    bool internalSignalingServerStarted = false;
    if (!useExternalSignaling) {
        const QString sfuExecutable = resolveInternalSfuExecutable();
        if (sfuExecutable.isEmpty()) {
            std::cout << "SKIP internal SFU binary not found" << std::endl;
            return finishProcessSmoke(tempDir, 77);
        }

        sfuRpcPort = reserveTcpPort();
        sfuMediaPort = reserveUdpPort();
        if (sfuRpcPort == 0 || sfuMediaPort == 0) {
            printFailure(QStringLiteral("failed to reserve SFU ports"));
            return finishProcessSmoke(tempDir, 1);
        }

        sfu.setProcessChannelMode(QProcess::ForwardedChannels);
        QProcessEnvironment sfuEnv = QProcessEnvironment::systemEnvironment();
        sfuEnv.insert(QStringLiteral("SFU_ADVERTISED_HOST"), QStringLiteral("127.0.0.1"));
        sfuEnv.insert(QStringLiteral("SFU_RPC_LISTEN_PORT"), QString::number(sfuRpcPort));
        sfuEnv.insert(QStringLiteral("SFU_MEDIA_LISTEN_PORT"), QString::number(sfuMediaPort));
        sfuEnv.insert(QStringLiteral("SFU_NODE_ID"), QStringLiteral("fanout-process-smoke-sfu"));
        sfu.setProcessEnvironment(sfuEnv);
        sfu.setWorkingDirectory(QFileInfo(sfuExecutable).absolutePath());
        sfu.start(sfuExecutable, {});
        if (!sfu.waitForStarted(10000)) {
            printFailure(QStringLiteral("failed to start internal SFU: %1").arg(sfu.errorString()));
            return finishProcessSmoke(tempDir, 1);
        }
        sfuRootPid = sfu.processId();
        internalSfuStarted = true;
        if (!waitForCondition(app, [sfuRpcPort] { return canConnect(QStringLiteral("127.0.0.1"), sfuRpcPort); }, 20000)) {
            printFailure(QStringLiteral("SFU RPC did not start listening"));
            printFailure(readAllOutput(sfu));
            return finishProcessSmoke(tempDir, 1);
        }

        server.setProcessChannelMode(QProcess::ForwardedChannels);
        QProcessEnvironment serverEnv = QProcessEnvironment::systemEnvironment();
        serverEnv.insert(QStringLiteral("SIGNALING_LISTEN_ADDR"), QStringLiteral("127.0.0.1:%1").arg(serverPort));
        serverEnv.insert(QStringLiteral("SIGNALING_ENABLE_REDIS"), QStringLiteral("false"));
        serverEnv.insert(QStringLiteral("SIGNALING_MYSQL_DSN"), QString());
        serverEnv.insert(QStringLiteral("SIGNALING_DEFAULT_SFU"), QStringLiteral("127.0.0.1:%1").arg(sfuMediaPort));
        serverEnv.insert(QStringLiteral("SIGNALING_SFU_RPC_ADDR"), QStringLiteral("127.0.0.1:%1").arg(sfuRpcPort));
        server.setProcessEnvironment(serverEnv);
        server.setWorkingDirectory(QStringLiteral("D:/meeting/meeting-server/signaling"));

        const QString goExecutable = resolveGoExecutable();
        QString serverStartedWith;
        const QStringList goCandidates = {goExecutable, QStringLiteral("go")};
        for (const auto& candidate : goCandidates) {
            if (candidate.trimmed().isEmpty()) {
                continue;
            }
            server.start(candidate, {QStringLiteral("run"), QStringLiteral(".")});
            if (server.waitForStarted(10000)) {
                serverStartedWith = candidate;
                break;
            }
        }
        if (serverStartedWith.isEmpty()) {
            printFailure(QStringLiteral("failed to start signaling server"));
            printFailure(readAllOutput(server));
            return finishProcessSmoke(tempDir, 1);
        }
        serverRootPid = server.processId();
        internalSignalingServerStarted = true;
    }

    const auto stopProcess = [](QProcess& process, const QString& name, qint64 rootPid = 0) {
        const qint64 processPid = process.processId() > 0 ? process.processId() : rootPid;
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(5000)) {
                process.kill();
                process.waitForFinished(5000);
            }
        }
#if defined(Q_OS_WIN)
        if (processPid > 0) {
            terminateProcessTree(static_cast<DWORD>(processPid));
        }
#endif
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
        const bool stopped = process.state() == QProcess::NotRunning;
        if (!stopped) {
            printFailure(QStringLiteral("cleanup failed: process=%1 pid=%2").arg(name).arg(processPid));
        }
        process.close();
        return stopped;
    };

    const auto stopInternalServices = [&]() {
        bool stopped = true;
        if (internalSignalingServerStarted) {
            stopped = stopProcess(server, QStringLiteral("signaling"), serverRootPid) && stopped;
        }
        if (internalSfuStarted) {
            stopped = stopProcess(sfu, QStringLiteral("sfu"), sfuRootPid) && stopped;
        }
        return stopped;
    };

    if (!waitForCondition(app, [signalingHost, serverPort] {
            return canConnect(signalingHost, serverPort);
        }, 20000)) {
        printFailure(QStringLiteral("signaling server did not start listening"));
        if (internalSignalingServerStarted) {
            printFailure(readAllOutput(server));
        }
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString clientExe = QDir(appDir).filePath(QStringLiteral("meeting_client.exe"));
    if (!QFileInfo::exists(clientExe)) {
        printFailure(QStringLiteral("meeting_client.exe not found at %1").arg(clientExe));
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }

    const QString meetingIdPath = tempDir.filePath(QStringLiteral("meeting_id.txt"));
    const QString hostResultPath = tempDir.filePath(QStringLiteral("host.result.txt"));
    const QString subscriberAResultPath = tempDir.filePath(QStringLiteral("subscriber_a.result.txt"));
    const QString subscriberBResultPath = tempDir.filePath(QStringLiteral("subscriber_b.result.txt"));

    auto buildClientEnv = [&](const ClientRuntime& client) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("MEETING_RUNTIME_SMOKE"), QStringLiteral("1"));
        env.insert(QStringLiteral("MEETING_SYNTHETIC_SCREEN"), QStringLiteral("1"));
        env.insert(QStringLiteral("MEETING_SERVER_HOST"), signalingHost);
        env.insert(QStringLiteral("MEETING_SERVER_PORT"), QString::number(serverPort));
        env.insert(QStringLiteral("MEETING_ADVERTISED_HOST"), signalingHost);
        env.insert(QStringLiteral("MEETING_SMOKE_ROLE"), client.role);
        env.insert(QStringLiteral("MEETING_SMOKE_USERNAME"), client.username);
        env.insert(QStringLiteral("MEETING_SMOKE_PASSWORD"), client.password);
        env.insert(QStringLiteral("MEETING_DB_PATH"), tempDir.filePath(client.dbName));
        env.insert(QStringLiteral("MEETING_SMOKE_MEETING_ID_PATH"), meetingIdPath);
        env.insert(QStringLiteral("MEETING_SMOKE_RESULT_PATH"), client.resultPath);
        env.insert(QStringLiteral("MEETING_SMOKE_TIMEOUT_MS"), QString::number(runtimeTimeoutMs));
        env.insert(QStringLiteral("MEETING_SMOKE_MEETING_CAPACITY"), QStringLiteral("3"));
        if (soakMs > 0) {
            env.insert(QStringLiteral("MEETING_SMOKE_SOAK_MS"), QString::number(soakMs));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_SOAK_MS"));
        }

        if (client.role == QStringLiteral("host")) {
            env.insert(QStringLiteral("MEETING_SMOKE_PEER_RESULT_PATHS"),
                       QStringLiteral("%1;%2").arg(subscriberAResultPath, subscriberBResultPath));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_PEER_RESULT_PATHS"));
        }

        env.insert(QStringLiteral("MEETING_SMOKE_ENABLE_LOCAL_VIDEO"), client.enableLocalVideo ? QStringLiteral("1")
                                                                                                : QStringLiteral("0"));
        env.insert(QStringLiteral("MEETING_SMOKE_DISABLE_LOCAL_VIDEO"), client.disableLocalVideo ? QStringLiteral("1")
                                                                                                  : QStringLiteral("0"));
        env.insert(QStringLiteral("MEETING_SMOKE_REQUIRE_VIDEO"), client.requireVideoEvidence ? QStringLiteral("1")
                                                                                               : QStringLiteral("0"));
        if (!client.observedRemoteVideoUserId.isEmpty()) {
            env.insert(QStringLiteral("MEETING_SMOKE_REMOTE_VIDEO_USER_ID"), client.observedRemoteVideoUserId);
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_REMOTE_VIDEO_USER_ID"));
        }

        const bool roleUsesSyntheticCamera = client.role != QStringLiteral("host") || syntheticCamera;
        if (roleUsesSyntheticCamera) {
            env.insert(QStringLiteral("MEETING_SYNTHETIC_CAMERA"), QStringLiteral("1"));
        } else {
            env.remove(QStringLiteral("MEETING_SYNTHETIC_CAMERA"));
        }

        if (client.expectCameraSource) {
            env.insert(QStringLiteral("MEETING_SMOKE_EXPECT_CAMERA_SOURCE"),
                       roleUsesSyntheticCamera ? QStringLiteral("synthetic-fallback")
                                               : QStringLiteral("real-device"));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_EXPECT_CAMERA_SOURCE"));
        }

        if (!client.cameraDevice.isEmpty()) {
            env.insert(QStringLiteral("MEETING_CAMERA_DEVICE_NAME"), client.cameraDevice);
        } else {
            env.remove(QStringLiteral("MEETING_CAMERA_DEVICE_NAME"));
        }

        env.insert(QStringLiteral("MEETING_SYNTHETIC_AUDIO"), QStringLiteral("1"));
        return env;
    };

    ClientRuntime host{
        QStringLiteral("host"),
        QStringLiteral("demo"),
        QStringLiteral("demo"),
        QStringLiteral("host.sqlite"),
        hostResultPath,
        tempDir.filePath(QStringLiteral("host.stdout.txt")),
        tempDir.filePath(QStringLiteral("host.stderr.txt")),
        QString(),
        hostCameraDevice,
        true,
        false,
        false,
        true
    };
    ClientRuntime subscriberA{
        QStringLiteral("subscriber_a"),
        QStringLiteral("alice"),
        QStringLiteral("alice"),
        QStringLiteral("subscriber_a.sqlite"),
        subscriberAResultPath,
        tempDir.filePath(QStringLiteral("subscriber_a.stdout.txt")),
        tempDir.filePath(QStringLiteral("subscriber_a.stderr.txt")),
        QStringLiteral("demo"),
        QString(),
        false,
        true,
        needVideoEvidence,
        false
    };
    ClientRuntime subscriberB{
        QStringLiteral("subscriber_b"),
        QStringLiteral("bob"),
        QStringLiteral("bob"),
        QStringLiteral("subscriber_b.sqlite"),
        subscriberBResultPath,
        tempDir.filePath(QStringLiteral("subscriber_b.stdout.txt")),
        tempDir.filePath(QStringLiteral("subscriber_b.stderr.txt")),
        QStringLiteral("demo"),
        QString(),
        false,
        true,
        needVideoEvidence,
        false
    };
    std::array<ClientRuntime*, 3> clients = {&host, &subscriberA, &subscriberB};

    const auto startClient = [&](ClientRuntime& client) {
        client.process.setWorkingDirectory(appDir);
        client.process.setProcessEnvironment(buildClientEnv(client));
        client.process.setStandardOutputFile(client.stdoutPath);
        client.process.setStandardErrorFile(client.stderrPath);
        client.process.start(clientExe, {});
        if (!client.process.waitForStarted(10000)) {
            printFailure(QStringLiteral("failed to start %1 client: %2").arg(client.role, client.process.errorString()));
            return false;
        }
        client.rootPid = client.process.processId();
        return true;
    };

    if (!startClient(host)) {
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }
    if (!waitForCondition(app,
                          [&meetingIdPath]() { return hasPublishedMeetingId(meetingIdPath); },
                          15000)) {
        printFailure(QStringLiteral("host did not publish meeting id in time"));
        printFailure(QStringLiteral("host result=\n%1").arg(readTextFile(host.resultPath)));
        stopProcess(host.process, host.role, host.rootPid);
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }

    if (!startClient(subscriberA)) {
        stopProcess(host.process, host.role, host.rootPid);
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }
    waitForCondition(app, [&subscriberA]() { return QFileInfo::exists(subscriberA.resultPath); }, 2000);

    if (!startClient(subscriberB)) {
        stopProcess(host.process, host.role, host.rootPid);
        stopProcess(subscriberA.process, subscriberA.role, subscriberA.rootPid);
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }

    bool success = true;
    for (ClientRuntime* client : clients) {
        success = waitForSuccessfulExit(app, client->process, timeoutMs) && success;
    }

    if (!success) {
        printFailure(QStringLiteral("fanout process smoke failed"));
        for (const ClientRuntime* client : clients) {
            const QString result = readTextFile(client->resultPath);
            printFailure(QStringLiteral("%1 status=%2 reason=%3")
                             .arg(client->role, resultStatusLine(result), resultReasonLine(result)));
            printFailure(QStringLiteral("%1 %2")
                             .arg(client->role, inferSmokeFailureBucket(resultStatusLine(result), resultReasonLine(result))));
            printFailure(QStringLiteral("%1 %2")
                             .arg(client->role, inferSmokeFailureStage(resultStatusLine(result), resultReasonLine(result))));
            printFailure(QStringLiteral("%1 exitStatus=%2 exitCode=%3 result=\n%4")
                             .arg(client->role)
                             .arg(client->process.exitStatus())
                             .arg(client->process.exitCode())
                             .arg(result));
            printFailure(QStringLiteral("%1 stdout:\n%2")
                             .arg(client->role, readTextFile(client->stdoutPath)));
            printFailure(QStringLiteral("%1 stderr:\n%2")
                             .arg(client->role, readTextFile(client->stderrPath)));
        }
        if (internalSignalingServerStarted) {
            printFailure(QStringLiteral("server:\n%1").arg(readAllOutput(server)));
        }
        if (internalSfuStarted) {
            printFailure(QStringLiteral("sfu:\n%1").arg(readAllOutput(sfu)));
        }
        for (ClientRuntime* client : clients) {
            stopProcess(client->process, client->role, client->rootPid);
        }
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }

    bool cleanupOk = true;
    for (ClientRuntime* client : clients) {
        cleanupOk = stopProcess(client->process, client->role, client->rootPid) && cleanupOk;
    }
    cleanupOk = stopInternalServices() && cleanupOk;
    if (!cleanupOk) {
        return finishProcessSmoke(tempDir, 1);
    }

    std::cout << "test_meeting_client_fanout_process_smoke passed" << std::endl;
    return finishProcessSmoke(tempDir, 0);
}
