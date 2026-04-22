#pragma once

#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QVideoFrame>

#include <QList>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace av::capture {

struct CameraCaptureFrame {
    QVideoFrame videoFrame;
    std::vector<uint8_t> bgra;
    int width{0};
    int height{0};
    int stride{0};

    bool hasVideoFrame() const {
        return videoFrame.isValid();
    }

    bool hasRawBgra() const {
        return width > 0 && height > 0 && stride >= width * 4 && !bgra.empty();
    }

    bool isValid() const {
        return hasVideoFrame() || hasRawBgra();
    }
};

class CameraCapture {
public:
    using FrameCallback = std::function<void(CameraCaptureFrame)>;

    CameraCapture();
    explicit CameraCapture(QCameraDevice device);
    explicit CameraCapture(const QString& deviceSelection);
    ~CameraCapture();

    static QList<QCameraDevice> availableDevices();
    static QStringList availableDeviceNames();
    static QString deviceName(const QCameraDevice& device);
    static QCameraDevice findDevice(const QString& selection);

    bool start();
    void stop();
    bool isRunning() const;

    bool setDevice(const QCameraDevice& device);
    bool setDeviceSelection(const QString& selection);
    void setFrameCallback(FrameCallback callback);
    QString lastError() const;
    bool usingDshowFallback() const;
    QString backendName() const;

private:
    struct Impl;

    std::shared_ptr<Impl> m_impl;
};

}  // namespace av::capture
