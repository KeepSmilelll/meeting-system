#include "CameraCapture.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtMultimedia/QCamera>
#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>

#include <atomic>
#include <mutex>
#include <utility>

namespace av::capture {

namespace {

QCameraDevice chooseDevice(const QCameraDevice& requested) {
    if (!requested.isNull()) {
        return requested;
    }

    const QCameraDevice defaultDevice = QMediaDevices::defaultVideoInput();
    if (!defaultDevice.isNull()) {
        return defaultDevice;
    }

    const auto inputs = QMediaDevices::videoInputs();
    if (!inputs.isEmpty()) {
        return inputs.front();
    }

    return {};
}

}  // namespace

struct CameraCapture::Impl {
    using FrameCallback = CameraCapture::FrameCallback;

    explicit Impl(QCameraDevice device = {})
        : requestedDevice(std::move(device)) {}

    static QList<QCameraDevice> availableDevices() {
        return QMediaDevices::videoInputs();
    }

    bool start(const std::shared_ptr<Impl>& self) {
        if (running.load(std::memory_order_acquire)) {
            return true;
        }

        if (QCoreApplication::instance() == nullptr) {
            qWarning().noquote() << "[camera-capture] no Qt application instance";
            return false;
        }

        const QCameraDevice device = chooseDevice(requestedDevice);
        if (device.isNull()) {
            qWarning().noquote() << "[camera-capture] no available camera device";
            return false;
        }

        auto newCamera = std::make_unique<QCamera>(device);
        auto newSink = std::make_unique<QVideoSink>();

        session.setVideoOutput(newSink.get());
        session.setCamera(newCamera.get());

        QObject::connect(newSink.get(), &QVideoSink::videoFrameChanged, newSink.get(), [self](const QVideoFrame& frame) {
            if (!self || !self->running.load(std::memory_order_acquire) || !frame.isValid()) {
                return;
            }

            FrameCallback callback;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                callback = self->frameCallback;
            }

            if (callback) {
                callback(frame);
            }
        });

        camera = std::move(newCamera);
        videoSink = std::move(newSink);
        running.store(true, std::memory_order_release);
        camera->start();
        return true;
    }

    void stop() {
        if (!running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        session.setVideoOutput(nullptr);
        session.setCamera(nullptr);

        if (camera) {
            camera->stop();
        }

        videoSink.reset();
        camera.reset();
    }

    void setFrameCallback(FrameCallback callback) {
        std::lock_guard<std::mutex> lock(mutex);
        frameCallback = std::move(callback);
    }

    bool setDevice(const QCameraDevice& device, const std::shared_ptr<Impl>& self) {
        const QCameraDevice nextDevice = chooseDevice(device);
        if (nextDevice.isNull()) {
            qWarning().noquote() << "[camera-capture] rejected empty camera device selection";
            return false;
        }

        const bool wasRunning = running.load(std::memory_order_acquire);
        const QCameraDevice previousDevice = requestedDevice;
        stop();
        requestedDevice = nextDevice;

        if (wasRunning && !start(self)) {
            qWarning().noquote() << "[camera-capture] failed to restart after device switch";
            requestedDevice = previousDevice;
            if (!start(self)) {
                qWarning().noquote() << "[camera-capture] failed to restore previous camera device";
            }
            return false;
        }

        return true;
    }

    QCameraDevice requestedDevice;
    std::unique_ptr<QCamera> camera;
    std::unique_ptr<QVideoSink> videoSink;
    QMediaCaptureSession session;
    std::mutex mutex;
    FrameCallback frameCallback;
    std::atomic<bool> running{false};
};

CameraCapture::CameraCapture()
    : m_impl(std::make_shared<Impl>()) {}

CameraCapture::CameraCapture(QCameraDevice device)
    : m_impl(std::make_shared<Impl>(std::move(device))) {}

CameraCapture::~CameraCapture() {
    stop();
}

QList<QCameraDevice> CameraCapture::availableDevices() {
    return Impl::availableDevices();
}

bool CameraCapture::start() {
    return m_impl ? m_impl->start(m_impl) : false;
}

void CameraCapture::stop() {
    if (m_impl) {
        m_impl->stop();
    }
}

bool CameraCapture::isRunning() const {
    return m_impl && m_impl->running.load(std::memory_order_acquire);
}

bool CameraCapture::setDevice(const QCameraDevice& device) {
    return m_impl ? m_impl->setDevice(device, m_impl) : false;
}

void CameraCapture::setFrameCallback(FrameCallback callback) {
    if (m_impl) {
        m_impl->setFrameCallback(std::move(callback));
    }
}

}  // namespace av::capture

