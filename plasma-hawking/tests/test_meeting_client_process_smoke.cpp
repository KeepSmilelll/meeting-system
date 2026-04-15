#include <iostream>
#include <algorithm>

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
#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#endif
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

bool allowSingleRealCameraMode() {
    return qEnvironmentVariableIntValue("MEETING_PROCESS_SMOKE_SINGLE_REAL_CAMERA") != 0;
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

bool requireDualVideoEvidence() {
    const QByteArray configured = qgetenv("MEETING_PROCESS_SMOKE_REQUIRE_VIDEO");
    if (configured == "1") {
        return true;
    }
    if (configured == "0") {
        return false;
    }
    return canEncodeVideoForProcessSmoke();
}

bool requireAudioEvidenceForProcessSmoke(bool syntheticAudio) {
    const QByteArray configured = qgetenv("MEETING_PROCESS_SMOKE_REQUIRE_AUDIO");
    if (configured == "1") {
        return true;
    }
    if (configured == "0") {
        return false;
    }
    return !syntheticAudio;
}

bool requireAvSyncEvidenceForProcessSmoke(bool requireVideoEvidence) {
    const QByteArray configured = qgetenv("MEETING_PROCESS_SMOKE_REQUIRE_AVSYNC");
    if (configured == "1") {
        return true;
    }
    if (configured == "0") {
        return false;
    }
    Q_UNUSED(requireVideoEvidence)
    return false;
}

int envIntValue(const char* key, int fallback) {
    bool ok = false;
    const int value = qEnvironmentVariable(key).toInt(&ok);
    return ok ? value : fallback;
}

double envDoubleValue(const char* key, double fallback) {
    bool ok = false;
    const double value = qEnvironmentVariable(key).toDouble(&ok);
    return ok ? value : fallback;
}

int processSmokeSoakDurationMs() {
    return (std::max)(0, envIntValue("MEETING_PROCESS_SMOKE_SOAK_MS", 0));
}

int processSmokeTimeoutMs(int soakDurationMs) {
    const int configured = (std::max)(10000, envIntValue("MEETING_PROCESS_SMOKE_TIMEOUT_MS", 45000));
    if (soakDurationMs <= 0) {
        return configured;
    }
    return (std::max)(configured, soakDurationMs + 120000);
}

int runtimeSmokeTimeoutMs(int soakDurationMs) {
    const int configured = (std::max)(30000, envIntValue("MEETING_PROCESS_SMOKE_RUNTIME_TIMEOUT_MS", 30000));
    if (soakDurationMs <= 0) {
        return configured;
    }
    return (std::max)(configured, soakDurationMs + 90000);
}

int processSmokeMaxWorkingSetGrowthMb(int soakDurationMs) {
    const int fallback = soakDurationMs > 0 ? 256 : 0;
    return (std::max)(0, envIntValue("MEETING_PROCESS_SMOKE_MAX_WORKING_SET_GROWTH_MB", fallback));
}

struct ProcessMemoryTracker {
    QString name;
    qint64 pid{0};
    quint64 baselineBytes{0};
    quint64 peakBytes{0};
    quint64 lastBytes{0};
    int samples{0};
    bool available{false};
};

struct ProcessCpuTracker {
    QString name;
    qint64 pid{0};
    quint64 lastCpuTime100ns{0};
    qint64 lastSampleTimeNs{0};
    double averagePercent{0.0};
    double peakPercent{0.0};
    double lastPercent{0.0};
    double totalPercent{0.0};
    quint64 percentSamples{0};
    bool available{false};
    bool hasPreviousSample{false};
};

#if defined(Q_OS_WIN)
quint64 queryWorkingSetBytes(qint64 pid) {
    if (pid <= 0) {
        return 0;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return 0;
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    const BOOL ok = GetProcessMemoryInfo(process,
                                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                         sizeof(counters));
    CloseHandle(process);
    if (!ok) {
        return 0;
    }

    return static_cast<quint64>(counters.WorkingSetSize);
}

quint64 fileTimeToUInt64(const FILETIME& value) {
    ULARGE_INTEGER merged{};
    merged.LowPart = value.dwLowDateTime;
    merged.HighPart = value.dwHighDateTime;
    return merged.QuadPart;
}

quint64 queryProcessCpuTime100ns(qint64 pid) {
    if (pid <= 0) {
        return 0;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return 0;
    }

    FILETIME createTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    const BOOL ok = GetProcessTimes(process, &createTime, &exitTime, &kernelTime, &userTime);
    CloseHandle(process);
    if (!ok) {
        return 0;
    }

    return fileTimeToUInt64(kernelTime) + fileTimeToUInt64(userTime);
}

quint32 querySystemLogicalCpuCount() {
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    return sysInfo.dwNumberOfProcessors > 0 ? static_cast<quint32>(sysInfo.dwNumberOfProcessors) : 1U;
}
#else
quint64 queryWorkingSetBytes(qint64) {
    return 0;
}

quint64 queryProcessCpuTime100ns(qint64) {
    return 0;
}

quint32 querySystemLogicalCpuCount() {
    return 1U;
}
#endif

void sampleProcessMemory(ProcessMemoryTracker& tracker, bool allowBaselineCapture = true) {
    const quint64 workingSetBytes = queryWorkingSetBytes(tracker.pid);
    if (workingSetBytes == 0) {
        return;
    }

    if (tracker.baselineBytes == 0) {
        if (!allowBaselineCapture) {
            return;
        }
        tracker.baselineBytes = workingSetBytes;
        tracker.peakBytes = workingSetBytes;
    }

    tracker.available = true;
    tracker.lastBytes = workingSetBytes;
    if (workingSetBytes > tracker.peakBytes) {
        tracker.peakBytes = workingSetBytes;
    }
    tracker.samples += 1;
}

void sampleProcessCpu(ProcessCpuTracker& tracker, qint64 sampleTimeNs, quint32 logicalCpuCount) {
    const quint64 processCpuTime100ns = queryProcessCpuTime100ns(tracker.pid);
    if (processCpuTime100ns == 0 || sampleTimeNs <= 0 || logicalCpuCount == 0U) {
        return;
    }

    if (!tracker.hasPreviousSample) {
        tracker.lastCpuTime100ns = processCpuTime100ns;
        tracker.lastSampleTimeNs = sampleTimeNs;
        tracker.hasPreviousSample = true;
        return;
    }

    const qint64 deltaWallNs = sampleTimeNs - tracker.lastSampleTimeNs;
    if (deltaWallNs <= 0) {
        return;
    }

    const quint64 deltaCpu100ns = processCpuTime100ns >= tracker.lastCpuTime100ns
                                      ? (processCpuTime100ns - tracker.lastCpuTime100ns)
                                      : 0U;
    tracker.lastCpuTime100ns = processCpuTime100ns;
    tracker.lastSampleTimeNs = sampleTimeNs;

    if (deltaCpu100ns == 0U) {
        return;
    }

    const double deltaWall100ns = static_cast<double>(deltaWallNs) / 100.0;
    if (deltaWall100ns <= 0.0) {
        return;
    }

    const double cpuPercent = (static_cast<double>(deltaCpu100ns) * 100.0) /
                              (deltaWall100ns * static_cast<double>(logicalCpuCount));
    tracker.available = true;
    tracker.lastPercent = cpuPercent;
    tracker.totalPercent += cpuPercent;
    ++tracker.percentSamples;
    tracker.averagePercent = tracker.totalPercent / static_cast<double>(tracker.percentSamples);
    if (cpuPercent > tracker.peakPercent) {
        tracker.peakPercent = cpuPercent;
    }
}

QString formatProcessMemory(const ProcessMemoryTracker& tracker) {
    if (!tracker.available) {
        return QStringLiteral("%1_working_set=unavailable").arg(tracker.name);
    }

    const auto toMb = [](quint64 bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    };

    return QStringLiteral("%1_working_set_mb=base:%2 peak:%3 last:%4 samples:%5")
        .arg(tracker.name)
        .arg(QString::number(toMb(tracker.baselineBytes), 'f', 1))
        .arg(QString::number(toMb(tracker.peakBytes), 'f', 1))
        .arg(QString::number(toMb(tracker.lastBytes), 'f', 1))
        .arg(tracker.samples);
}

QString formatProcessCpu(const ProcessCpuTracker& tracker) {
    if (!tracker.available || tracker.percentSamples == 0) {
        return QStringLiteral("%1_cpu_percent=unavailable").arg(tracker.name);
    }

    return QStringLiteral("%1_cpu_percent=avg:%2 peak:%3 last:%4 samples:%5")
        .arg(tracker.name)
        .arg(QString::number(tracker.averagePercent, 'f', 2))
        .arg(QString::number(tracker.peakPercent, 'f', 2))
        .arg(QString::number(tracker.lastPercent, 'f', 2))
        .arg(tracker.percentSamples);
}

bool exceedsWorkingSetGrowth(const ProcessMemoryTracker& tracker, quint64 maxGrowthBytes) {
    if (!tracker.available || tracker.baselineBytes == 0 || tracker.peakBytes <= tracker.baselineBytes) {
        return false;
    }
    return (tracker.peakBytes - tracker.baselineBytes) > maxGrowthBytes;
}

bool exceedsCpuBudget(const ProcessCpuTracker& tracker, double maxCpuPercent) {
    if (!tracker.available || tracker.percentSamples == 0 || maxCpuPercent < 0.0) {
        return false;
    }
    return tracker.averagePercent > maxCpuPercent;
}

bool exceedsCpuPeakBudget(const ProcessCpuTracker& tracker, double maxCpuPeakPercent) {
    if (!tracker.available || tracker.percentSamples == 0 || maxCpuPeakPercent < 0.0) {
        return false;
    }
    return tracker.peakPercent > maxCpuPeakPercent;
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
    if (normalizedStatus == QStringLiteral("WAITING_AVSYNC")) {
        return QStringLiteral("likely-stage=avsync-evidence");
    }
    if (normalizedStatus == QStringLiteral("WAITING_CAMERA_SOURCE")) {
        return QStringLiteral("likely-stage=camera-source-evidence");
    }
    if (normalizedStatus == QStringLiteral("WAITING_AUDIO")) {
        return QStringLiteral("likely-stage=audio-evidence");
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
    if (normalizedStatus == QStringLiteral("WAITING_AVSYNC")) {
        return QStringLiteral("likely-module=avsync-audio-clock");
    }
    if (normalizedStatus == QStringLiteral("WAITING_CAMERA_SOURCE")) {
        return QStringLiteral("likely-module=video-capture-source");
    }
    if (normalizedStatus == QStringLiteral("WAITING_AUDIO")) {
        return QStringLiteral("likely-module=audio-capture-or-transport");
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

bool waitForSuccessfulExit(QCoreApplication& app,
                           QProcess& process,
                           int timeoutMs,
                           const std::function<void()>& tick = {}) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        app.processEvents(QEventLoop::AllEvents, 20);
        if (tick) {
            tick();
        }
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
    const bool singleRealCameraMode = allowSingleRealCameraMode();
    const bool requireVideoEvidence = requireDualVideoEvidence();
    const bool requireAudioEvidence = requireAudioEvidenceForProcessSmoke(syntheticAudio);
    const bool requireAvSyncEvidence = requireAvSyncEvidenceForProcessSmoke(requireVideoEvidence);
    const bool requireCpuEvidence = envIntValue("MEETING_PROCESS_SMOKE_REQUIRE_CPU", 0) != 0;
    const int avSyncMaxSkewMs = (std::max)(0, envIntValue("MEETING_PROCESS_SMOKE_AVSYNC_MAX_SKEW_MS", 40));
    const double maxCpuPercent = (std::max)(0.0, envDoubleValue("MEETING_PROCESS_SMOKE_MAX_CPU_PERCENT", 15.0));
    const double maxCpuPeakPercent = (std::max)(0.0, envDoubleValue("MEETING_PROCESS_SMOKE_MAX_CPU_PEAK_PERCENT", 50.0));
    const int cpuVideoWidth = (std::max)(320, envIntValue("MEETING_PROCESS_SMOKE_CPU_VIDEO_WIDTH", 1920));
    const int cpuVideoHeight = (std::max)(180, envIntValue("MEETING_PROCESS_SMOKE_CPU_VIDEO_HEIGHT", 1080));
    const int cpuVideoFps = (std::max)(1, envIntValue("MEETING_PROCESS_SMOKE_CPU_VIDEO_FPS", 30));
    const int cpuVideoBitrateBps = (std::max)(300000, envIntValue("MEETING_PROCESS_SMOKE_CPU_VIDEO_BITRATE_BPS", 4000000));
    const int soakDurationMs = processSmokeSoakDurationMs();
    const int waitTimeoutMs = processSmokeTimeoutMs(soakDurationMs);
    const int runtimeTimeoutForClientMs = runtimeSmokeTimeoutMs(soakDurationMs);
    const int maxWorkingSetGrowthMb = processSmokeMaxWorkingSetGrowthMb(soakDurationMs);
    const int memoryBaselineDelayMs = (std::max)(0, envIntValue("MEETING_PROCESS_SMOKE_MEMORY_BASELINE_DELAY_MS", soakDurationMs > 0 ? 15000 : 0));
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
        const bool useSyntheticCameraForRole =
            syntheticCamera || (singleRealCameraMode && role == QStringLiteral("guest"));
        if (useSyntheticCameraForRole) {
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
        env.insert(QStringLiteral("MEETING_SMOKE_TIMEOUT_MS"), QString::number(runtimeTimeoutForClientMs));
        if (soakDurationMs > 0) {
            env.insert(QStringLiteral("MEETING_SMOKE_SOAK_MS"), QString::number(soakDurationMs));
            env.insert(QStringLiteral("MEETING_SMOKE_SOAK_POLL_INTERVAL_MS"), QStringLiteral("1000"));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_SOAK_MS"));
            env.remove(QStringLiteral("MEETING_SMOKE_SOAK_POLL_INTERVAL_MS"));
        }
        if (requireVideoEvidence) {
            env.insert(QStringLiteral("MEETING_SMOKE_REQUIRE_VIDEO"), QStringLiteral("1"));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_REQUIRE_VIDEO"));
        }
        if (requireAvSyncEvidence) {
            env.insert(QStringLiteral("MEETING_SMOKE_REQUIRE_AVSYNC"), QStringLiteral("1"));
            env.insert(QStringLiteral("MEETING_SMOKE_AVSYNC_MAX_SKEW_MS"), QString::number(avSyncMaxSkewMs));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_REQUIRE_AVSYNC"));
            env.remove(QStringLiteral("MEETING_SMOKE_AVSYNC_MAX_SKEW_MS"));
        }
        if (requireAudioEvidence) {
            env.insert(QStringLiteral("MEETING_SMOKE_REQUIRE_AUDIO"), QStringLiteral("1"));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_REQUIRE_AUDIO"));
        }
        if (requireCpuEvidence) {
            env.insert(QStringLiteral("MEETING_VIDEO_WIDTH"), QString::number(cpuVideoWidth));
            env.insert(QStringLiteral("MEETING_VIDEO_HEIGHT"), QString::number(cpuVideoHeight));
            env.insert(QStringLiteral("MEETING_VIDEO_FPS"), QString::number(cpuVideoFps));
            env.insert(QStringLiteral("MEETING_VIDEO_BITRATE_BPS"), QString::number(cpuVideoBitrateBps));
        } else {
            env.remove(QStringLiteral("MEETING_VIDEO_WIDTH"));
            env.remove(QStringLiteral("MEETING_VIDEO_HEIGHT"));
            env.remove(QStringLiteral("MEETING_VIDEO_FPS"));
            env.remove(QStringLiteral("MEETING_VIDEO_BITRATE_BPS"));
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

    ProcessMemoryTracker hostMemory{QStringLiteral("host"), host.processId()};
    ProcessMemoryTracker guestMemory{QStringLiteral("guest"), guest.processId()};
    ProcessCpuTracker hostCpu{QStringLiteral("host"), host.processId()};
    ProcessCpuTracker guestCpu{QStringLiteral("guest"), guest.processId()};
    const quint32 logicalCpuCount = querySystemLogicalCpuCount();
    QElapsedTimer memorySamplingTimer;
    memorySamplingTimer.start();
    QElapsedTimer cpuSamplingTimer;
    cpuSamplingTimer.start();
    const auto sampleRuntimeStats = [&hostMemory,
                                     &guestMemory,
                                     &hostCpu,
                                     &guestCpu,
                                     &memorySamplingTimer,
                                     &cpuSamplingTimer,
                                     memoryBaselineDelayMs,
                                     logicalCpuCount]() {
        const bool allowBaselineCapture = memorySamplingTimer.elapsed() >= memoryBaselineDelayMs;
        sampleProcessMemory(hostMemory, allowBaselineCapture);
        sampleProcessMemory(guestMemory, allowBaselineCapture);
        const qint64 sampleTimeNs = cpuSamplingTimer.nsecsElapsed();
        sampleProcessCpu(hostCpu, sampleTimeNs, logicalCpuCount);
        sampleProcessCpu(guestCpu, sampleTimeNs, logicalCpuCount);
    };

    sampleRuntimeStats();
    const bool hostOk = waitForSuccessfulExit(app, host, waitTimeoutMs, sampleRuntimeStats);
    const bool guestOk = waitForSuccessfulExit(app, guest, waitTimeoutMs, sampleRuntimeStats);
    sampleRuntimeStats();

    if (!hostOk || !guestOk) {
        const QString hostResult = readTextFile(hostResultPath);
        const QString guestResult = readTextFile(guestResultPath);
        printFailure(QStringLiteral("process smoke failed"));
        printFailure(QStringLiteral("video evidence required=%1 encoder available=%2")
                         .arg(requireVideoEvidence ? 1 : 0)
                         .arg(canEncodeVideoForProcessSmoke() ? 1 : 0));
        printFailure(QStringLiteral("avsync evidence required=%1 max_skew_ms=%2")
                         .arg(requireAvSyncEvidence ? 1 : 0)
                         .arg(avSyncMaxSkewMs));
        printFailure(QStringLiteral("audio evidence required=%1 synthetic_audio=%2")
                         .arg(requireAudioEvidence ? 1 : 0)
                         .arg(syntheticAudio ? 1 : 0));
        printFailure(QStringLiteral("cpu evidence required=%1 max_cpu_percent=%2 logical_cpu_count=%3 cpu_video=%4x%5@%6 bitrate_bps=%7")
                         .arg(requireCpuEvidence ? 1 : 0)
                         .arg(QString::number(maxCpuPercent, 'f', 2))
                         .arg(logicalCpuCount)
                         .arg(cpuVideoWidth)
                         .arg(cpuVideoHeight)
                         .arg(cpuVideoFps)
                         .arg(cpuVideoBitrateBps));
        printFailure(QStringLiteral("single_real_camera_mode=%1")
                         .arg(singleRealCameraMode ? 1 : 0));
        printFailure(QStringLiteral("cpu peak threshold percent=%1")
                         .arg(QString::number(maxCpuPeakPercent, 'f', 2)));
        printFailure(QStringLiteral("soak_ms=%1 process_timeout_ms=%2 runtime_timeout_ms=%3 max_ws_growth_mb=%4 memory_baseline_delay_ms=%5")
                         .arg(soakDurationMs)
                         .arg(waitTimeoutMs)
                         .arg(runtimeTimeoutForClientMs)
                         
                         .arg(maxWorkingSetGrowthMb)
                         .arg(memoryBaselineDelayMs));
        printFailure(formatProcessMemory(hostMemory));
        printFailure(formatProcessMemory(guestMemory));
        printFailure(formatProcessCpu(hostCpu));
        printFailure(formatProcessCpu(guestCpu));
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

    if (soakDurationMs > 0 && maxWorkingSetGrowthMb > 0) {
        const quint64 maxGrowthBytes = static_cast<quint64>(maxWorkingSetGrowthMb) * 1024ULL * 1024ULL;
        const bool hostGrowthExceeded = exceedsWorkingSetGrowth(hostMemory, maxGrowthBytes);
        const bool guestGrowthExceeded = exceedsWorkingSetGrowth(guestMemory, maxGrowthBytes);
        if (hostGrowthExceeded || guestGrowthExceeded) {
            printFailure(QStringLiteral("process smoke leak guard failed: working set growth exceeded threshold"));
            printFailure(QStringLiteral("threshold_mb=%1 memory_baseline_delay_ms=%2")
                         .arg(maxWorkingSetGrowthMb)
                         .arg(memoryBaselineDelayMs));
            printFailure(formatProcessMemory(hostMemory));
            printFailure(formatProcessMemory(guestMemory));
            stopProcess(server);
            return 1;
        }
    }
    if (requireCpuEvidence) {
        const bool hostCpuExceeded = exceedsCpuBudget(hostCpu, maxCpuPercent);
        const bool guestCpuExceeded = exceedsCpuBudget(guestCpu, maxCpuPercent);
        const bool hostCpuPeakExceeded = exceedsCpuPeakBudget(hostCpu, maxCpuPeakPercent);
        const bool guestCpuPeakExceeded = exceedsCpuPeakBudget(guestCpu, maxCpuPeakPercent);
        if (hostCpuExceeded || guestCpuExceeded || hostCpuPeakExceeded || guestCpuPeakExceeded) {
            printFailure(QStringLiteral("process smoke cpu guard failed: CPU exceeded threshold"));
            printFailure(QStringLiteral("max_cpu_percent=%1 max_cpu_peak_percent=%2 logical_cpu_count=%3 cpu_video=%4x%5@%6 bitrate_bps=%7")
                             .arg(QString::number(maxCpuPercent, 'f', 2))
                             .arg(QString::number(maxCpuPeakPercent, 'f', 2))
                             .arg(logicalCpuCount)
                             .arg(cpuVideoWidth)
                             .arg(cpuVideoHeight)
                             .arg(cpuVideoFps)
                             .arg(cpuVideoBitrateBps));
            printFailure(formatProcessCpu(hostCpu));
            printFailure(formatProcessCpu(guestCpu));
            printFailure(formatProcessMemory(hostMemory));
            printFailure(formatProcessMemory(guestMemory));
            stopProcess(server);
            return 1;
        }
    }
    std::cout << formatProcessMemory(hostMemory).toLocal8Bit().constData() << std::endl;
    std::cout << formatProcessMemory(guestMemory).toLocal8Bit().constData() << std::endl;
    std::cout << formatProcessCpu(hostCpu).toLocal8Bit().constData() << std::endl;
    std::cout << formatProcessCpu(guestCpu).toLocal8Bit().constData() << std::endl;

    stopProcess(server);
    std::cout << "test_meeting_client_process_smoke passed" << std::endl;
    return 0;
}

