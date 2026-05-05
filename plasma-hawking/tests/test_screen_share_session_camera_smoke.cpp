#include "av/session/ScreenShareSession.h"
#include "av/VideoPipelineProfile.h"
#include "av/capture/WindowsD3D11CameraCapture.h"
#include "av/codec/VideoEncoder.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <iostream>

namespace {

QString envValue(const char* key, const QString& fallback = QString()) {
    const QString value = qEnvironmentVariable(key);
    return value.isEmpty() ? fallback : value;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    const QString deviceSelection = envValue("MEETING_TEST_CAMERA_DEVICE");
    if (deviceSelection.trimmed().isEmpty()) {
        std::cerr << "SKIP missing MEETING_TEST_CAMERA_DEVICE" << std::endl;
        return 77;
    }

    av::session::ScreenShareSessionConfig config;
    config.localAddress = "127.0.0.1";
    config.peerAddress = "127.0.0.1";
    config.localPort = 0;
    config.peerPort = 65000;
    config.width = 1280;
    config.height = 720;
    config.frameRate = 30;
    config.bitrate = 2500000;

    if (av::isHardwareE2E(av::videoPipelineProfileFromEnvironment())) {
        av::codec::VideoEncoder encoder;
        if (!encoder.configureHardwareD3D11(config.width,
                                            config.height,
                                            config.frameRate,
                                            config.bitrate,
                                            config.cameraPayloadType,
                                            av::codec::VideoEncoderPreset::Realtime)) {
            std::cerr << "SKIP hardware D3D11 encoder unavailable" << std::endl;
            return 77;
        }

        av::capture::WindowsD3D11CameraCapture cameraCapture;
        std::string error;
        if (!cameraCapture.initialize(encoder,
                                      deviceSelection.toStdString(),
                                      config.width,
                                      config.height,
                                      config.frameRate,
                                      &error)) {
            std::cerr << "SKIP hardware camera unsupported " << error << std::endl;
            return 77;
        }

        for (int i = 0; i < 300; ++i) {
            av::AVFramePtr frame;
            error.clear();
            if (cameraCapture.capture(encoder, i, frame, &error)) {
                if (!frame ||
                    static_cast<AVPixelFormat>(frame->format) != AV_PIX_FMT_D3D11 ||
                    frame->hw_frames_ctx == nullptr) {
                    std::cerr << "hardware-camera-invalid-frame" << std::endl;
                    return 1;
                }
                std::cerr << "hardware-camera-frame-observed cameraBackend=mf-d3d11 cameraInterop=dxgi" << std::endl;
                return 0;
            }
            if (!error.empty()) {
                std::cerr << "SKIP hardware camera unsupported " << error << std::endl;
                return 77;
            }
            QThread::msleep(10);
        }
        std::cerr << "hardware-camera-frame-timeout" << std::endl;
        return 3;
    }

    av::session::ScreenShareSession session(config);
    session.setPreferredCameraDeviceName(deviceSelection.toStdString());
    session.setStatusCallback([&](std::string message) {
        const QString text = QString::fromStdString(message);
        std::cerr << "status " << message << std::endl;
        if (text.contains(QStringLiteral("Video camera frame observed"), Qt::CaseInsensitive)) {
            QTimer::singleShot(0, qApp, []() {
                QCoreApplication::exit(0);
            });
        }
    });
    session.setErrorCallback([&](std::string message) {
        std::cerr << "screen-session-error " << message << std::endl;
        QTimer::singleShot(0, qApp, []() {
            QCoreApplication::exit(2);
        });
    });

    std::cerr << "phase session-start" << std::endl;
    if (!session.start()) {
        std::cerr << "session-start-failed " << session.lastError() << std::endl;
        return 1;
    }
    std::cerr << "phase camera-enable" << std::endl;
    if (!session.setCameraSendingEnabled(true)) {
        std::cerr << "camera-enable-failed " << session.lastError() << std::endl;
        session.stop();
        return 1;
    }
    std::cerr << "phase camera-enabled" << std::endl;

    QTimer::singleShot(10000, &app, [&]() {
        std::cerr << "camera-frame-timeout " << session.lastError() << std::endl;
        QCoreApplication::exit(3);
    });

    const int exitCode = app.exec();
    session.stop();
    return exitCode;
}
