#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <qqml.h>
#include <QtGlobal>
#include <QVariant>
#include <memory>

#include "app/AppStateMachine.h"
#include "app/MeetingController.h"
#include "app/RuntimeSmokeDriver.h"
#include "av/render/VideoRenderer.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    qmlRegisterType<av::render::VideoRenderer>("MeetingApp", 1, 0, "VideoRenderer");
    MeetingController controller(qEnvironmentVariable("MEETING_DB_PATH"));
    AppStateMachine appStateMachine;
    std::unique_ptr<RuntimeSmokeDriver> smokeDriver;
    engine.setInitialProperties({
        {QStringLiteral("meetingController"), QVariant::fromValue(&controller)},
        {QStringLiteral("appStateMachine"), QVariant::fromValue(&appStateMachine)},
    });

    const QUrl mainUrl(QStringLiteral("qrc:/MeetingApp/qml/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() {
        QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(mainUrl);
    smokeDriver = std::make_unique<RuntimeSmokeDriver>(&controller, &app);
    if (smokeDriver->enabled() && !engine.rootObjects().isEmpty()) {
        smokeDriver->start();
    }
    return app.exec();
}

