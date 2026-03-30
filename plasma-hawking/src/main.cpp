#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "app/MeetingController.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    MeetingController controller;
    engine.rootContext()->setContextProperty("meetingController", &controller);

    const QUrl mainUrl(QStringLiteral("qrc:/MeetingApp/qml/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() {
        QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(mainUrl);
    return app.exec();
}
