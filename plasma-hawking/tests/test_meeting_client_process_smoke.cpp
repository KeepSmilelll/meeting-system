#include <iostream>

#include <functional>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QMediaDevices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDebug>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

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

bool hasDefaultAudioDevices() {
    return !QMediaDevices::defaultAudioInput().isNull() &&
           !QMediaDevices::defaultAudioOutput().isNull();
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

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        printFailure(QStringLiteral("failed to create temp dir"));
        return 1;
    }

    const bool syntheticAudio = useSyntheticAudio();
    if (!syntheticAudio && !hasDefaultAudioDevices()) {
        std::cout << "SKIP no default audio input/output available" << std::endl;
        return 77;
    }

    const quint16 serverPort = reserveTcpPort();
    if (serverPort == 0) {
        printFailure(QStringLiteral("failed to reserve tcp port"));
        return 1;
    }

    QProcess server;
    QProcessEnvironment serverEnv = QProcessEnvironment::systemEnvironment();
    serverEnv.insert(QStringLiteral("SIGNALING_LISTEN_ADDR"), QStringLiteral("127.0.0.1:%1").arg(serverPort));
    serverEnv.insert(QStringLiteral("SIGNALING_ENABLE_REDIS"), QStringLiteral("false"));
    serverEnv.insert(QStringLiteral("SIGNALING_MYSQL_DSN"), QString());
    server.setProcessEnvironment(serverEnv);
    server.setWorkingDirectory(QStringLiteral("D:/meeting/meeting-server/signaling"));
    server.start(QStringLiteral("go"), {QStringLiteral("run"), QStringLiteral(".")});
    if (!server.waitForStarted(10000)) {
        printFailure(QStringLiteral("failed to start signaling server"));
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
        printFailure(QStringLiteral("server did not listen in time\n%1").arg(readAllOutput(server)));
        stopProcess(server);
        return 1;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString clientExe = QDir(appDir).filePath(QStringLiteral("meeting_client.exe"));
    if (!QFileInfo::exists(clientExe)) {
        printFailure(QStringLiteral("meeting_client.exe not found at %1").arg(clientExe));
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
        return env;
    };

    QProcess host;
    host.setProcessEnvironment(buildClientEnv(QStringLiteral("host"), QStringLiteral("demo"), QStringLiteral("demo"), QStringLiteral("host.sqlite"), hostResultPath, guestResultPath));
    host.setWorkingDirectory(appDir);
    host.start(clientExe, {});
    if (!host.waitForStarted(10000)) {
        printFailure(QStringLiteral("failed to start host client"));
        stopProcess(server);
        return 1;
    }

    QProcess guest;
    guest.setProcessEnvironment(buildClientEnv(QStringLiteral("guest"), QStringLiteral("alice"), QStringLiteral("alice"), QStringLiteral("guest.sqlite"), guestResultPath, hostResultPath));
    guest.setWorkingDirectory(appDir);
    guest.start(clientExe, {});
    if (!guest.waitForStarted(10000)) {
        printFailure(QStringLiteral("failed to start guest client"));
        stopProcess(host);
        stopProcess(server);
        return 1;
    }

    const bool hostOk = waitForSuccessfulExit(app, host, 45000);
    const bool guestOk = waitForSuccessfulExit(app, guest, 45000);

    if (!hostOk || !guestOk) {
        printFailure(QStringLiteral("process smoke failed"));
        printFailure(QStringLiteral("host exitStatus=%1 exitCode=%2 result=\n%3")
                         .arg(host.exitStatus())
                         .arg(host.exitCode())
                         .arg(readTextFile(hostResultPath)));
        printFailure(QStringLiteral("guest exitStatus=%1 exitCode=%2 result=\n%3")
                         .arg(guest.exitStatus())
                         .arg(guest.exitCode())
                         .arg(readTextFile(guestResultPath)));
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
