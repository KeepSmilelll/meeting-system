#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QVariant>

#include "app/AppStateMachine.h"
#include "app/MeetingController.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    MeetingController controller;
    AppStateMachine appStateMachine;
    engine.setInitialProperties({
        {QStringLiteral("meetingController"), QVariant::fromValue(&controller)},
        {QStringLiteral("appStateMachine"), QVariant::fromValue(&appStateMachine)},
    });

    const QUrl mainUrl(QStringLiteral("qrc:/MeetingApp/qml/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() {
        QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(mainUrl);
    return app.exec();
}

