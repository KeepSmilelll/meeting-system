#include <functional>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QThread>
#include <QStringList>
#include <QDebug>

#include "app/MeetingController.h"

namespace {

constexpr quint16 kServerPort = 18443;

struct ControllerProbe {
    QStringList infoMessages;
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

bool canConnectToServer() {
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), kServerPort);
    const bool connected = socket.waitForConnected(200);
    if (connected) {
        socket.disconnectFromHost();
    }
    return connected;
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

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qputenv("MEETING_SYNTHETIC_AUDIO", "1");

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        qCritical().noquote() << "failed to create temporary directory";
        return 1;
    }

    QProcess server;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SIGNALING_LISTEN_ADDR"), QStringLiteral("127.0.0.1:%1").arg(kServerPort));
    env.insert(QStringLiteral("SIGNALING_ENABLE_REDIS"), QStringLiteral("false"));
    env.insert(QStringLiteral("SIGNALING_MYSQL_DSN"), QString());
    server.setProcessEnvironment(env);
    server.setWorkingDirectory(QStringLiteral("D:/meeting/meeting-server/signaling"));
    server.start(QStringLiteral("go"), {QStringLiteral("run"), QStringLiteral(".")});
    if (!server.waitForStarted(10000)) {
        qCritical().noquote() << "failed to start signaling server";
        return 1;
    }

    const auto stopServer = [&server]() {
        server.terminate();
        if (!server.waitForFinished(5000)) {
            server.kill();
            server.waitForFinished(5000);
        }
    };

    if (!waitForCondition(app, [] { return canConnectToServer(); }, 20000)) {
        qCritical().noquote() << "signaling server did not start listening\n" << collectedOutput(server);
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

    hostController.setServerEndpoint(QStringLiteral("127.0.0.1"), kServerPort);
    guestController.setServerEndpoint(QStringLiteral("127.0.0.1"), kServerPort);

    hostController.login(QStringLiteral("demo"), QStringLiteral("demo"));
    guestController.login(QStringLiteral("alice"), QStringLiteral("alice"));

    if (!waitForCondition(app, [&hostController, &guestController] {
            return hostController.loggedIn() && guestController.loggedIn();
        }, 10000)) {
        qCritical().noquote() << "login smoke failed";
        qCritical().noquote() << "host status:" << hostController.statusText();
        qCritical().noquote() << "guest status:" << guestController.statusText();
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    hostController.createMeeting(QStringLiteral("runtime-smoke"), QString(), 2);
    if (!waitForCondition(app, [&hostController] {
            return hostController.inMeeting() && !hostController.meetingId().isEmpty();
        }, 10000)) {
        qCritical().noquote() << "host createMeeting smoke failed";
        qCritical().noquote() << "host status:" << hostController.statusText();
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    guestController.joinMeeting(hostController.meetingId(), QString());
    if (!waitForCondition(app, [&hostController, &guestController] {
            return guestController.inMeeting() &&
                   guestController.meetingId() == hostController.meetingId() &&
                   hostController.participants().size() >= 2 &&
                   guestController.participants().size() >= 2;
        }, 10000)) {
        qCritical().noquote() << "guest joinMeeting smoke failed";
        qCritical().noquote() << "host status:" << hostController.statusText();
        qCritical().noquote() << "guest status:" << guestController.statusText();
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    if (!waitForCondition(app, [&hostProbe, &guestProbe] {
            return (containsMessage(hostProbe.infoMessages, QStringLiteral("Audio offer sent")) ||
                    containsMessage(hostProbe.infoMessages, QStringLiteral("Video offer sent"))) &&
                   (containsMessage(guestProbe.infoMessages, QStringLiteral("Audio answer sent")) ||
                    containsMessage(guestProbe.infoMessages, QStringLiteral("Video answer sent")));
        }, 15000)) {
        qCritical().noquote() << "media negotiation smoke failed";
        qCritical().noquote() << "host messages:" << hostProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << "guest messages:" << guestProbe.infoMessages.join(QStringLiteral(" | "));
        qCritical().noquote() << collectedOutput(server);
        stopServer();
        return 1;
    }

    if (containsMessage(hostProbe.infoMessages, QStringLiteral("Failed")) ||
        containsMessage(guestProbe.infoMessages, QStringLiteral("Failed"))) {
        qCritical().noquote() << "runtime smoke observed failure messages";
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
