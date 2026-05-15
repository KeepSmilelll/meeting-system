#include "av/capture/ScreenCapture.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QThread>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>

namespace {

int envInt(const char* name, int fallback) {
    bool ok = false;
    const int value = qEnvironmentVariable(name).toInt(&ok);
    return ok ? value : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    qunsetenv("MEETING_SYNTHETIC_SCREEN");

    QGuiApplication app(argc, argv);
    if (QGuiApplication::primaryScreen() == nullptr) {
        std::cerr << "portal-screen-smoke skipped: no primary screen" << std::endl;
        return 77;
    }

    const int timeoutMs = std::max(5000, envInt("MEETING_PORTAL_SCREEN_SMOKE_TIMEOUT_MS", 45000));
    av::capture::ScreenCapture capture(av::capture::ScreenCaptureConfig{
        640,
        360,
        10,
        3,
    });

    if (!capture.start()) {
        std::cerr << "portal-screen-smoke failed: start returned false" << std::endl;
        return 1;
    }

    QElapsedTimer timer;
    timer.start();
    av::capture::ScreenFrame frame;
    while (timer.elapsed() < timeoutMs) {
        app.processEvents(QEventLoop::AllEvents, 20);
        if (capture.popFrameForEncode(frame, std::chrono::milliseconds(20))) {
            capture.stop();
            const bool hasPixels = frame.width > 0 &&
                                   frame.height > 0 &&
                                   frame.bgra.size() >=
                                       static_cast<std::size_t>(frame.width) *
                                           static_cast<std::size_t>(frame.height) * 4U;
            if (!hasPixels) {
                std::cerr << "portal-screen-smoke failed: invalid frame "
                          << frame.width << "x" << frame.height
                          << " bytes=" << frame.bgra.size() << std::endl;
                return 2;
            }
            std::cout << "portal-screen-smoke frame width=" << frame.width
                      << " height=" << frame.height
                      << " pts=" << frame.pts
                      << " bgra=" << frame.bgra.size() << std::endl;
            return 0;
        }
        if (!capture.isRunning()) {
            std::cerr << "portal-screen-smoke failed: capture stopped before first frame" << std::endl;
            capture.stop();
            return 3;
        }
        QThread::msleep(10);
    }

    capture.stop();
    std::cerr << "portal-screen-smoke failed: timed out waiting for first frame after "
              << timeoutMs << "ms" << std::endl;
    return 4;
}
