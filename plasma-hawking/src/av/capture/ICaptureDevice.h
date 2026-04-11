#pragma once

#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QVideoFrame>

#include <QList>

#include <functional>
#include <memory>

namespace av::capture {

class CameraCapture {
public:
    using FrameCallback = std::function<void(QVideoFrame)>;

    CameraCapture();
    explicit CameraCapture(QCameraDevice device);
    ~CameraCapture();

    static QList<QCameraDevice> availableDevices();

    bool start();
    void stop();
    bool isRunning() const;

    bool setDevice(const QCameraDevice& device);
    void setFrameCallback(FrameCallback callback);

private:
    struct Impl;

    std::shared_ptr<Impl> m_impl;
};

}  // namespace av::capture
