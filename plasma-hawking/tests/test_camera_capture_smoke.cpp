#include "av/capture/CameraCapture.h"

#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace {

QString envValue(const char* key, const QString& fallback = QString()) {
    const QString value = qEnvironmentVariable(key);
    return value.isEmpty() ? fallback : value;
}

bool envFlag(const char* key) {
    return qEnvironmentVariableIntValue(key) != 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    const QString deviceSelection = envValue("MEETING_TEST_CAMERA_DEVICE");
    if (deviceSelection.trimmed().isEmpty()) {
        std::cerr << "SKIP missing MEETING_TEST_CAMERA_DEVICE" << std::endl;
        return 77;
    }

    const QString backend = envValue("MEETING_CAMERA_CAPTURE_BACKEND", QStringLiteral("auto"));
    qputenv("MEETING_CAMERA_CAPTURE_BACKEND", backend.toUtf8());

    av::capture::CameraCapture capture(deviceSelection);
    std::atomic<int> frameCount{0};
    capture.setFrameCallback([&frameCount](av::capture::CameraCaptureFrame frame) {
        if (!frame.isValid()) {
            return;
        }
        frameCount.fetch_add(1, std::memory_order_acq_rel);
        QTimer::singleShot(0, qApp, []() {
            QCoreApplication::exit(0);
        });
    });

    if (!capture.start()) {
        std::cerr << "camera-start-failed backend=" << capture.backendName().toStdString()
                  << " error=" << capture.lastError().toStdString() << std::endl;
        return 1;
    }

    const int timeoutMs = envFlag("MEETING_TEST_CAMERA_LONG_TIMEOUT") ? 15000 : 5000;
    QTimer::singleShot(timeoutMs, &app, [&capture, &frameCount]() {
        std::cerr << "camera-timeout backend=" << capture.backendName().toStdString()
                  << " frames=" << frameCount.load(std::memory_order_acquire)
                  << " error=" << capture.lastError().toStdString() << std::endl;
        QCoreApplication::exit(2);
    });

    const int exitCode = app.exec();
    capture.stop();
    return exitCode;
}
