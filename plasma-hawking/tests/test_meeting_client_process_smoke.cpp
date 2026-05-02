#include <iostream>
#include <algorithm>

#include <functional>
#include <vector>

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
#include <QUdpSocket>
#include <QThread>
#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif
#include "av/capture/CameraCapture.h"
#include "av/codec/VideoEncoder.h"

namespace {

int finishProcessSmoke(QTemporaryDir& tempDir, int exitCode) {
    std::cout.flush();
    std::cerr.flush();
    tempDir.remove();
#if defined(Q_OS_WIN)
    // Qt Multimedia can leave process-global backend threads alive after this
    // harness has already stopped its children. End the test process directly
    // so CTest observes the real result instead of timing out in teardown.
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
        if (candidate.isEmpty()) {
            continue;
        }
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
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

bool allowSingleRealAudioMode() {
    return qEnvironmentVariableIntValue("MEETING_PROCESS_SMOKE_SINGLE_REAL_AUDIO") != 0;
}

bool hasDefaultAudioDevices() {
    if (!QMediaDevices::defaultAudioInput().isNull() &&
        !QMediaDevices::defaultAudioOutput().isNull()) {
        return true;
    }

    // In headless smoke harnesses, QMediaDevices can fail to surface defaults
    // even when explicit device routing works inside spawned meeting clients.
    const QString hostInput = qEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_AUDIO_INPUT_DEVICE").trimmed();
    const QString hostOutput = qEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_AUDIO_OUTPUT_DEVICE").trimmed();
    const QString guestInput = qEnvironmentVariable("MEETING_PROCESS_SMOKE_GUEST_AUDIO_INPUT_DEVICE").trimmed();
    const QString guestOutput = qEnvironmentVariable("MEETING_PROCESS_SMOKE_GUEST_AUDIO_OUTPUT_DEVICE").trimmed();
    return !hostInput.isEmpty() || !hostOutput.isEmpty() || !guestInput.isEmpty() || !guestOutput.isEmpty();
}

QStringList availableAudioInputDevicesForSmoke() {
    QStringList names;
    const auto inputs = QMediaDevices::audioInputs();
    names.reserve(inputs.size());
    for (const auto& input : inputs) {
        const QString description = input.description().trimmed();
        names.push_back(description.isEmpty() ? QString::fromUtf8(input.id()).trimmed() : description);
    }
    names.removeDuplicates();
    return names;
}

QStringList availableAudioOutputDevicesForSmoke() {
    QStringList names;
    const auto outputs = QMediaDevices::audioOutputs();
    names.reserve(outputs.size());
    for (const auto& output : outputs) {
        const QString description = output.description().trimmed();
        names.push_back(description.isEmpty() ? QString::fromUtf8(output.id()).trimmed() : description);
    }
    names.removeDuplicates();
    return names;
}

bool containsDeviceName(const QStringList& devices, const QString& preferredDeviceName) {
    const QString preferred = preferredDeviceName.trimmed();
    if (preferred.isEmpty()) {
        return true;
    }

    for (const QString& device : devices) {
        if (device.compare(preferred, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool explicitAudioDeviceMissing(const QString& label,
                                const QString& preferredDeviceName,
                                const QStringList& availableDevices) {
    if (containsDeviceName(availableDevices, preferredDeviceName)) {
        return false;
    }

    std::cout << "SKIP explicit audio " << label.toStdString()
              << " device not found; requested=" << preferredDeviceName.toStdString()
              << " available=" << availableDevices.join(QStringLiteral(", ")).toStdString()
              << std::endl;
    return true;
}

bool hasVideoInputDevices() {
    if (!QMediaDevices::videoInputs().isEmpty()) {
        return true;
    }

    if (!av::capture::CameraCapture::availableDeviceNames().isEmpty()) {
        return true;
    }

    // Allow explicit camera pinning to bypass harness-level enumeration gaps.
    const QString hostCamera = qEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_CAMERA_DEVICE").trimmed();
    const QString guestCamera = qEnvironmentVariable("MEETING_PROCESS_SMOKE_GUEST_CAMERA_DEVICE").trimmed();
    return !hostCamera.isEmpty() || !guestCamera.isEmpty();
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

    const QString fallback = QStringLiteral("D:\\go-env\\go\\bin\\go.exe");
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
        return finishProcessSmoke(tempDir, 1);
    }

    const bool syntheticAudio = useSyntheticAudio();
    const bool syntheticCamera = useSyntheticCamera();
    const bool singleRealCameraMode = allowSingleRealCameraMode();
    const bool singleRealAudioMode = allowSingleRealAudioMode();
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
    const QString hostCameraDevice = qEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_CAMERA_DEVICE").trimmed();
    const QString guestCameraDevice = qEnvironmentVariable("MEETING_PROCESS_SMOKE_GUEST_CAMERA_DEVICE").trimmed();
    const QString hostAudioInputDevice = qEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_AUDIO_INPUT_DEVICE").trimmed();
    const QString hostAudioOutputDevice = qEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_AUDIO_OUTPUT_DEVICE").trimmed();
    const QString guestAudioInputDevice = qEnvironmentVariable("MEETING_PROCESS_SMOKE_GUEST_AUDIO_INPUT_DEVICE").trimmed();
    const QString guestAudioOutputDevice = qEnvironmentVariable("MEETING_PROCESS_SMOKE_GUEST_AUDIO_OUTPUT_DEVICE").trimmed();
    const int soakDurationMs = processSmokeSoakDurationMs();
    const int waitTimeoutMs = processSmokeTimeoutMs(soakDurationMs);
    const int runtimeTimeoutForClientMs = runtimeSmokeTimeoutMs(soakDurationMs);
    const int maxWorkingSetGrowthMb = processSmokeMaxWorkingSetGrowthMb(soakDurationMs);
    const int memoryBaselineDelayMs = (std::max)(0, envIntValue("MEETING_PROCESS_SMOKE_MEMORY_BASELINE_DELAY_MS", soakDurationMs > 0 ? 15000 : 0));

    if (!syntheticAudio) {
        const QStringList availableInputs = availableAudioInputDevicesForSmoke();
        const QStringList availableOutputs = availableAudioOutputDevicesForSmoke();
        if (explicitAudioDeviceMissing(QStringLiteral("host input"), hostAudioInputDevice, availableInputs) ||
            explicitAudioDeviceMissing(QStringLiteral("guest input"), guestAudioInputDevice, availableInputs) ||
            explicitAudioDeviceMissing(QStringLiteral("host output"), hostAudioOutputDevice, availableOutputs) ||
            explicitAudioDeviceMissing(QStringLiteral("guest output"), guestAudioOutputDevice, availableOutputs)) {
            return finishProcessSmoke(tempDir, 77);
        }
    }

    if (!syntheticAudio && !hasDefaultAudioDevices()) {
        std::cout << "SKIP no default audio input/output available" << std::endl;
        return finishProcessSmoke(tempDir, 77);
    }

    if (!syntheticAudio &&
        !singleRealAudioMode &&
        hostAudioInputDevice.isEmpty() &&
        guestAudioInputDevice.isEmpty()) {
        const QStringList availableInputs = availableAudioInputDevicesForSmoke();
        if (availableInputs.size() < 2) {
            std::cout << "SKIP dual real-audio smoke requires two capture devices or single-real-audio mode; "
                      << "available_inputs=" << availableInputs.join(QStringLiteral(", ")).toStdString()
                      << std::endl;
            return finishProcessSmoke(tempDir, 77);
        }
    }

    if (!syntheticCamera && !hasVideoInputDevices()) {
        std::cout << "SKIP no camera device available for real-camera mode" << std::endl;
        return finishProcessSmoke(tempDir, 77);
    }

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
            printFailure(QStringLiteral("failed to reserve tcp port"));
            printStageHint(QStringLiteral("bootstrap_port_reserve"), QStringLiteral("runtime-bootstrap"));
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
            printStageHint(QStringLiteral("sfu_port_reserve"), QStringLiteral("runtime-bootstrap"));
            return finishProcessSmoke(tempDir, 1);
        }

        sfu.setProcessChannelMode(QProcess::ForwardedChannels);
        QProcessEnvironment sfuEnv = QProcessEnvironment::systemEnvironment();
        sfuEnv.insert(QStringLiteral("SFU_ADVERTISED_HOST"), signalingHost);
        sfuEnv.insert(QStringLiteral("SFU_RPC_LISTEN_PORT"), QString::number(sfuRpcPort));
        sfuEnv.insert(QStringLiteral("SFU_MEDIA_LISTEN_PORT"), QString::number(sfuMediaPort));
        sfuEnv.insert(QStringLiteral("SFU_NODE_ID"), QStringLiteral("process-smoke-sfu"));
        sfu.setProcessEnvironment(sfuEnv);
        sfu.setWorkingDirectory(QFileInfo(sfuExecutable).absolutePath());
        sfu.start(sfuExecutable);
        if (!sfu.waitForStarted(10000)) {
            const QString startMessage = QStringLiteral("failed to start internal SFU: %1").arg(sfu.errorString());
            printFailure(startMessage);
            printStageHint(QStringLiteral("sfu_server_start"), QStringLiteral("runtime-bootstrap"), startMessage);
            return finishProcessSmoke(tempDir, 1);
        }
        sfuRootPid = sfu.processId();
        internalSfuStarted = true;
        if (!waitForCondition(app, [signalingHost, sfuRpcPort] {
                return canConnect(signalingHost, sfuRpcPort);
            }, 20000)) {
            QString listenError = QStringLiteral("internal SFU RPC did not listen in time");
            listenError += QStringLiteral("\n%1").arg(readAllOutput(sfu));
            printFailure(listenError);
            printStageHint(QStringLiteral("sfu_server_listen"), QStringLiteral("runtime-bootstrap"), listenError);
            return finishProcessSmoke(tempDir, 1);
        }

        server.setProcessChannelMode(QProcess::ForwardedChannels);
        QProcessEnvironment serverEnv = QProcessEnvironment::systemEnvironment();
        serverEnv.insert(QStringLiteral("SIGNALING_LISTEN_ADDR"), QStringLiteral("%1:%2").arg(signalingHost).arg(serverPort));
        serverEnv.insert(QStringLiteral("SIGNALING_ENABLE_REDIS"), QStringLiteral("false"));
        serverEnv.insert(QStringLiteral("SIGNALING_MYSQL_DSN"), QString());
        serverEnv.insert(QStringLiteral("SIGNALING_DEFAULT_SFU"), QStringLiteral("%1:%2").arg(signalingHost).arg(sfuMediaPort));
        serverEnv.insert(QStringLiteral("SIGNALING_SFU_RPC_ADDR"), QStringLiteral("%1:%2").arg(signalingHost).arg(sfuRpcPort));
        server.setProcessEnvironment(serverEnv);
        server.setWorkingDirectory(QStringLiteral("D:/meeting/meeting-server/signaling"));
        const QString resolvedGoExecutable = resolveGoExecutable();
        const QStringList goCandidates = {
            resolvedGoExecutable,
            QDir::toNativeSeparators(resolvedGoExecutable),
            QStringLiteral("go")
        };
        QString startError;
        QString startedWith;
        for (const QString& goExecutable : goCandidates) {
            if (goExecutable.trimmed().isEmpty()) {
                continue;
            }
            server.start(goExecutable, {QStringLiteral("run"), QStringLiteral(".")});
            if (server.waitForStarted(10000)) {
                startedWith = goExecutable;
                serverRootPid = server.processId();
                internalSignalingServerStarted = true;
                break;
            }
            const QString candidateError = QStringLiteral("%1 => %2").arg(goExecutable, server.errorString());
            if (startError.isEmpty()) {
                startError = candidateError;
            } else if (!startError.contains(candidateError)) {
                startError += QStringLiteral(" | %1").arg(candidateError);
            }
        }
        if (startedWith.isEmpty()) {
            const QString startMessage = QStringLiteral("failed to start signaling server, tried: %1")
                                             .arg(startError.isEmpty() ? resolvedGoExecutable : startError);
            printFailure(startMessage);
            printStageHint(QStringLiteral("signaling_server_start"), QStringLiteral("signaling-or-negotiation"), startMessage);
            return finishProcessSmoke(tempDir, 1);
        }
        std::cout << "signaling_start_command=" << startedWith.toLocal8Bit().constData() << std::endl;
    } else {
        std::cout << QStringLiteral("using external signaling at %1:%2")
                         .arg(signalingHost)
                         .arg(serverPort)
                         .toLocal8Bit()
                         .constData()
                  << std::endl;
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
            printFailure(QStringLiteral("cleanup failed: process=%1 pid=%2 state=%3 error=%4")
                             .arg(name)
                             .arg(processPid)
                             .arg(static_cast<int>(process.state()))
                             .arg(process.errorString()));
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
        QString listenError = QStringLiteral("server did not listen in time");
        if (internalSignalingServerStarted) {
            listenError += QStringLiteral("\n%1").arg(readAllOutput(server));
        }
        printFailure(listenError);
        printStageHint(QStringLiteral("signaling_server_listen"), QStringLiteral("signaling-or-negotiation"), listenError);
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString clientExe = QDir(appDir).filePath(QStringLiteral("meeting_client.exe"));
    if (!QFileInfo::exists(clientExe)) {
        const QString clientMissing = QStringLiteral("meeting_client.exe not found at %1").arg(clientExe);
        printFailure(clientMissing);
        printStageHint(QStringLiteral("client_binary_lookup"), QStringLiteral("runtime-bootstrap"), clientMissing);
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
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
        const bool useSyntheticAudioForRole =
            syntheticAudio || (singleRealAudioMode && role == QStringLiteral("guest"));
        if (useSyntheticCameraForRole) {
            env.insert(QStringLiteral("MEETING_SYNTHETIC_CAMERA"), QStringLiteral("1"));
            env.insert(QStringLiteral("MEETING_SMOKE_EXPECT_CAMERA_SOURCE"), QStringLiteral("synthetic-fallback"));
            env.remove(QStringLiteral("MEETING_SMOKE_EXPECT_REAL_CAMERA"));
        } else {
            env.remove(QStringLiteral("MEETING_SYNTHETIC_CAMERA"));
            env.insert(QStringLiteral("MEETING_SMOKE_EXPECT_CAMERA_SOURCE"), QStringLiteral("real-device"));
            env.remove(QStringLiteral("MEETING_SMOKE_EXPECT_REAL_CAMERA"));
        }
        const QString preferredCameraDevice = role == QStringLiteral("host") ? hostCameraDevice :
                                              (role == QStringLiteral("guest") ? guestCameraDevice : QString{});
        if (!preferredCameraDevice.isEmpty()) {
            env.insert(QStringLiteral("MEETING_CAMERA_DEVICE_NAME"), preferredCameraDevice);
        } else {
            env.remove(QStringLiteral("MEETING_CAMERA_DEVICE_NAME"));
        }
        if (useSyntheticAudioForRole) {
            env.insert(QStringLiteral("MEETING_SYNTHETIC_AUDIO"), QStringLiteral("1"));
            env.remove(QStringLiteral("MEETING_AUDIO_INPUT_DEVICE_NAME"));
            env.remove(QStringLiteral("MEETING_AUDIO_OUTPUT_DEVICE_NAME"));
        } else {
            env.remove(QStringLiteral("MEETING_SYNTHETIC_AUDIO"));
            const QString preferredAudioInputDevice = role == QStringLiteral("host") ? hostAudioInputDevice :
                                                      (role == QStringLiteral("guest") ? guestAudioInputDevice : QString{});
            const QString preferredAudioOutputDevice = role == QStringLiteral("host") ? hostAudioOutputDevice :
                                                       (role == QStringLiteral("guest") ? guestAudioOutputDevice : QString{});
            if (!preferredAudioInputDevice.isEmpty()) {
                env.insert(QStringLiteral("MEETING_AUDIO_INPUT_DEVICE_NAME"), preferredAudioInputDevice);
            } else {
                env.remove(QStringLiteral("MEETING_AUDIO_INPUT_DEVICE_NAME"));
            }
            if (!preferredAudioOutputDevice.isEmpty()) {
                env.insert(QStringLiteral("MEETING_AUDIO_OUTPUT_DEVICE_NAME"), preferredAudioOutputDevice);
            } else {
                env.remove(QStringLiteral("MEETING_AUDIO_OUTPUT_DEVICE_NAME"));
            }
        }
        env.insert(QStringLiteral("MEETING_SMOKE_ROLE"), role);
        env.insert(QStringLiteral("MEETING_SMOKE_USERNAME"), username);
        env.insert(QStringLiteral("MEETING_SMOKE_PASSWORD"), password);
        env.insert(QStringLiteral("MEETING_ADVERTISED_HOST"), signalingHost);
        env.insert(QStringLiteral("MEETING_SERVER_HOST"), signalingHost);
        env.insert(QStringLiteral("MEETING_SERVER_PORT"), QString::number(serverPort));
        env.insert(QStringLiteral("MEETING_DB_PATH"), tempDir.filePath(dbName));
        env.insert(QStringLiteral("MEETING_SMOKE_MEETING_ID_PATH"), meetingIdPath);
        env.insert(QStringLiteral("MEETING_SMOKE_RESULT_PATH"), resultPath);
        env.insert(QStringLiteral("MEETING_SMOKE_PEER_RESULT_PATH"), peerResultPath);
        env.insert(QStringLiteral("MEETING_SMOKE_TIMEOUT_MS"), QString::number(runtimeTimeoutForClientMs));
        env.insert(QStringLiteral("MEETING_SMOKE_ENABLE_LOCAL_AUDIO"), QStringLiteral("1"));
        env.insert(QStringLiteral("MEETING_SMOKE_DISABLE_LOCAL_AUDIO"), QStringLiteral("0"));
        env.insert(QStringLiteral("MEETING_SMOKE_ENABLE_LOCAL_VIDEO"), QStringLiteral("1"));
        env.insert(QStringLiteral("MEETING_SMOKE_DISABLE_LOCAL_VIDEO"), QStringLiteral("0"));
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
            env.insert(QStringLiteral("MEETING_VIDEO_AUDIO_DRIVEN_MAX_DELAY_MS"), QStringLiteral("20"));
            env.insert(QStringLiteral("MEETING_VIDEO_RENDER_QUEUE_DEPTH"), QStringLiteral("4"));
            env.insert(QStringLiteral("MEETING_VIDEO_MAX_FRAMES_PER_DRAIN"), QStringLiteral("4"));
            env.insert(QStringLiteral("MEETING_VIDEO_MAX_CADENCE_MS"), QStringLiteral("40"));
        } else {
            env.remove(QStringLiteral("MEETING_SMOKE_REQUIRE_AVSYNC"));
            env.remove(QStringLiteral("MEETING_SMOKE_AVSYNC_MAX_SKEW_MS"));
            env.remove(QStringLiteral("MEETING_VIDEO_AUDIO_DRIVEN_MAX_DELAY_MS"));
            env.remove(QStringLiteral("MEETING_VIDEO_RENDER_QUEUE_DEPTH"));
            env.remove(QStringLiteral("MEETING_VIDEO_MAX_FRAMES_PER_DRAIN"));
            env.remove(QStringLiteral("MEETING_VIDEO_MAX_CADENCE_MS"));
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
    host.setProcessChannelMode(QProcess::ForwardedChannels);
    host.setProcessEnvironment(buildClientEnv(QStringLiteral("host"), QStringLiteral("demo"), QStringLiteral("demo"), QStringLiteral("host.sqlite"), hostResultPath, guestResultPath));
    host.setWorkingDirectory(appDir);
    host.start(clientExe, {});
    if (!host.waitForStarted(10000)) {
        printFailure(QStringLiteral("failed to start host client"));
        printStageHint(QStringLiteral("host_process_start"), QStringLiteral("runtime-bootstrap"));
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }
    const qint64 hostRootPid = host.processId();

    QProcess guest;
    guest.setProcessChannelMode(QProcess::ForwardedChannels);
    guest.setProcessEnvironment(buildClientEnv(QStringLiteral("guest"), QStringLiteral("alice"), QStringLiteral("alice"), QStringLiteral("guest.sqlite"), guestResultPath, hostResultPath));
    guest.setWorkingDirectory(appDir);
    guest.start(clientExe, {});
    if (!guest.waitForStarted(10000)) {
        printFailure(QStringLiteral("failed to start guest client"));
        printStageHint(QStringLiteral("guest_process_start"), QStringLiteral("runtime-bootstrap"));
        stopProcess(host, QStringLiteral("host"), hostRootPid);
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
    }
    const qint64 guestRootPid = guest.processId();

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
        printFailure(QStringLiteral("single_real_audio_mode=%1")
                         .arg(singleRealAudioMode ? 1 : 0));
        printFailure(QStringLiteral("camera_device_override host=%1 guest=%2")
                         .arg(hostCameraDevice.isEmpty() ? QStringLiteral("<default>") : hostCameraDevice,
                              guestCameraDevice.isEmpty() ? QStringLiteral("<default>") : guestCameraDevice));
        printFailure(QStringLiteral("audio_device_override host_in=%1 host_out=%2 guest_in=%3 guest_out=%4")
                         .arg(hostAudioInputDevice.isEmpty() ? QStringLiteral("<default>") : hostAudioInputDevice,
                              hostAudioOutputDevice.isEmpty() ? QStringLiteral("<default>") : hostAudioOutputDevice,
                              guestAudioInputDevice.isEmpty() ? QStringLiteral("<default>") : guestAudioInputDevice,
                              guestAudioOutputDevice.isEmpty() ? QStringLiteral("<default>") : guestAudioOutputDevice));
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
        if (internalSignalingServerStarted) {
            printFailure(QStringLiteral("server:\n%1").arg(readAllOutput(server)));
        }
        stopProcess(host, QStringLiteral("host"), hostRootPid);
        stopProcess(guest, QStringLiteral("guest"), guestRootPid);
        stopInternalServices();
        return finishProcessSmoke(tempDir, 1);
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
            stopProcess(host, QStringLiteral("host"), hostRootPid);
            stopProcess(guest, QStringLiteral("guest"), guestRootPid);
            stopInternalServices();
            return finishProcessSmoke(tempDir, 1);
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
            stopProcess(host, QStringLiteral("host"), hostRootPid);
            stopProcess(guest, QStringLiteral("guest"), guestRootPid);
            stopInternalServices();
            return finishProcessSmoke(tempDir, 1);
        }
    }
    std::cout << formatProcessMemory(hostMemory).toLocal8Bit().constData() << std::endl;
    std::cout << formatProcessMemory(guestMemory).toLocal8Bit().constData() << std::endl;
    std::cout << formatProcessCpu(hostCpu).toLocal8Bit().constData() << std::endl;
    std::cout << formatProcessCpu(guestCpu).toLocal8Bit().constData() << std::endl;
    std::cout << QStringLiteral("camera_device_override host=%1 guest=%2")
                     .arg(hostCameraDevice.isEmpty() ? QStringLiteral("<default>") : hostCameraDevice,
                          guestCameraDevice.isEmpty() ? QStringLiteral("<default>") : guestCameraDevice)
                     .toLocal8Bit()
                     .constData()
              << std::endl;
    std::cout << QStringLiteral("audio_device_override host_in=%1 host_out=%2 guest_in=%3 guest_out=%4")
                     .arg(hostAudioInputDevice.isEmpty() ? QStringLiteral("<default>") : hostAudioInputDevice,
                          hostAudioOutputDevice.isEmpty() ? QStringLiteral("<default>") : hostAudioOutputDevice,
                          guestAudioInputDevice.isEmpty() ? QStringLiteral("<default>") : guestAudioInputDevice,
                          guestAudioOutputDevice.isEmpty() ? QStringLiteral("<default>") : guestAudioOutputDevice)
                     .toLocal8Bit()
                     .constData()
              << std::endl;

    bool cleanupOk = true;
    cleanupOk = stopProcess(host, QStringLiteral("host"), hostRootPid) && cleanupOk;
    cleanupOk = stopProcess(guest, QStringLiteral("guest"), guestRootPid) && cleanupOk;
    cleanupOk = stopInternalServices() && cleanupOk;
    if (!cleanupOk) {
        return finishProcessSmoke(tempDir, 1);
    }
    std::cout << "test_meeting_client_process_smoke passed" << std::endl;
    return finishProcessSmoke(tempDir, 0);
}

