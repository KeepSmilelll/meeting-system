#include "ScreenShareSession.h"

#include "VideoSessionControlActions.h"
#include "VideoSessionStateMachine.h"
#include "VideoThreadLifecycleStateMachine.h"

#include "av/capture/CameraCapture.h"
#include "av/capture/ScreenCapture.h"

#include <QDebug>
#include <QImage>
#include <QMutexLocker>
#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QVideoFrame>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>

namespace av::session {
namespace {

uint32_t makeSsrc() {
    uint32_t value = 0;
    while (value == 0) {
        value = static_cast<uint32_t>(std::random_device{}());
    }
    return value;
}

uint32_t chooseConfiguredOrRandomSsrc(uint32_t configuredSsrc) {
    return configuredSsrc != 0U ? configuredSsrc : makeSsrc();
}

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool allowSyntheticCameraFallback() {
    return qEnvironmentVariableIntValue("MEETING_SYNTHETIC_CAMERA") != 0;
}

bool debugCameraRelayEnabled() {
    return qEnvironmentVariableIntValue("MEETING_DEBUG_CAMERA_RELAY") != 0;
}

void debugCameraRelayTrace(const QString& message) {
    if (!debugCameraRelayEnabled()) {
        return;
    }
    std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    std::fflush(stderr);
}

QString normalizeCameraDeviceName(const QString& name) {
    return name.trimmed();
}

QString cameraDeviceLabel(const QCameraDevice& device) {
    const QString description = normalizeCameraDeviceName(device.description());
    if (!description.isEmpty()) {
        return description;
    }
    return QString::fromUtf8(device.id()).trimmed();
}

QCameraDevice resolvePreferredCameraDeviceName(const QString& preferredDeviceName) {
    const QString normalized = preferredDeviceName.trimmed();
    if (normalized.isEmpty()) {
        return {};
    }

    const auto inputs = QMediaDevices::videoInputs();
    for (const auto& input : inputs) {
        if (input.description().compare(normalized, Qt::CaseInsensitive) == 0 ||
            QString::fromUtf8(input.id()).compare(normalized, Qt::CaseInsensitive) == 0) {
            return input;
        }
    }

    for (const auto& input : inputs) {
        if (input.description().contains(normalized, Qt::CaseInsensitive) ||
            QString::fromUtf8(input.id()).contains(normalized, Qt::CaseInsensitive)) {
            return input;
        }
    }

    return {};
}

uint8_t clampToByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint8_t lumaFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    return clampToByte((((66 * static_cast<int>(r)) +
                         (129 * static_cast<int>(g)) +
                         (25 * static_cast<int>(b)) +
                         128) >> 8) + 16);
}

uint8_t chromaUFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    return clampToByte((((-38 * static_cast<int>(r)) -
                         (74 * static_cast<int>(g)) +
                         (112 * static_cast<int>(b)) +
                         128) >> 8) + 128);
}

uint8_t chromaVFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    return clampToByte((((112 * static_cast<int>(r)) -
                         (94 * static_cast<int>(g)) -
                         (18 * static_cast<int>(b)) +
                         128) >> 8) + 128);
}

bool fillNv12PreviewFromArgb32Image(const QImage& image, av::codec::DecodedVideoFrame& outFrame) {
    if (image.isNull() || image.format() != QImage::Format_ARGB32 ||
        image.width() <= 0 || image.height() <= 0 ||
        (image.width() % 2) != 0 || (image.height() % 2) != 0) {
        return false;
    }

    outFrame = av::codec::DecodedVideoFrame{};
    outFrame.width = image.width();
    outFrame.height = image.height();
    outFrame.pixelFormat = AV_PIX_FMT_NV12;
    outFrame.yPlane.resize(static_cast<std::size_t>(outFrame.width) * static_cast<std::size_t>(outFrame.height));
    outFrame.uvPlane.resize(static_cast<std::size_t>(outFrame.width) * static_cast<std::size_t>(outFrame.height / 2));

    for (int y = 0; y < outFrame.height; ++y) {
        const uint8_t* srcRow = image.constScanLine(y);
        auto* yRow = outFrame.yPlane.data() + static_cast<std::ptrdiff_t>(y) * outFrame.width;
        for (int x = 0; x < outFrame.width; ++x) {
            const uint8_t* pixel = srcRow + static_cast<std::ptrdiff_t>(x) * 4;
            yRow[x] = lumaFromBgra(pixel[0], pixel[1], pixel[2]);
        }
    }

    for (int y = 0; y < outFrame.height; y += 2) {
        const uint8_t* srcRow0 = image.constScanLine(y);
        const uint8_t* srcRow1 = image.constScanLine(std::min(y + 1, outFrame.height - 1));
        auto* uvRow = outFrame.uvPlane.data() + static_cast<std::ptrdiff_t>(y / 2) * outFrame.width;
        for (int x = 0; x < outFrame.width; x += 2) {
            const uint8_t* p00 = srcRow0 + static_cast<std::ptrdiff_t>(x) * 4;
            const uint8_t* p01 = srcRow0 + static_cast<std::ptrdiff_t>(std::min(x + 1, outFrame.width - 1)) * 4;
            const uint8_t* p10 = srcRow1 + static_cast<std::ptrdiff_t>(x) * 4;
            const uint8_t* p11 = srcRow1 + static_cast<std::ptrdiff_t>(std::min(x + 1, outFrame.width - 1)) * 4;

            const int uAvg = (static_cast<int>(chromaUFromBgra(p00[0], p00[1], p00[2])) +
                              static_cast<int>(chromaUFromBgra(p01[0], p01[1], p01[2])) +
                              static_cast<int>(chromaUFromBgra(p10[0], p10[1], p10[2])) +
                              static_cast<int>(chromaUFromBgra(p11[0], p11[1], p11[2])) +
                              2) / 4;
            const int vAvg = (static_cast<int>(chromaVFromBgra(p00[0], p00[1], p00[2])) +
                              static_cast<int>(chromaVFromBgra(p01[0], p01[1], p01[2])) +
                              static_cast<int>(chromaVFromBgra(p10[0], p10[1], p10[2])) +
                              static_cast<int>(chromaVFromBgra(p11[0], p11[1], p11[2])) +
                              2) / 4;

            uvRow[x] = clampToByte(uAvg);
            uvRow[x + 1] = clampToByte(vAvg);
        }
    }

    return true;
}

bool buildLocalCameraPreviewFrame(const av::capture::CameraCaptureFrame& frame,
                                  int targetWidth,
                                  int targetHeight,
                                  av::codec::DecodedVideoFrame& outFrame) {
    targetWidth = std::max(2, targetWidth & ~1);
    targetHeight = std::max(2, targetHeight & ~1);

    QImage image;
    if (frame.hasRawBgra()) {
        image = QImage(reinterpret_cast<const uchar*>(frame.bgra.data()),
                       frame.width,
                       frame.height,
                       frame.stride,
                       QImage::Format_ARGB32).copy();
    } else if (frame.hasVideoFrame()) {
        QVideoFrame cpuFrame(frame.videoFrame);
        image = cpuFrame.toImage();
        if (image.isNull() && cpuFrame.map(QVideoFrame::ReadOnly)) {
            image = cpuFrame.toImage();
            cpuFrame.unmap();
        }
    }

    if (image.isNull()) {
        return false;
    }
    if (image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32);
    }
    if (image.width() != targetWidth || image.height() != targetHeight) {
        image = image.scaled(targetWidth, targetHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (image.format() != QImage::Format_ARGB32) {
            image = image.convertToFormat(QImage::Format_ARGB32);
        }
    }
    if (image.isNull()) {
        return false;
    }

    return fillNv12PreviewFromArgb32Image(image, outFrame);
}

}  // namespace

bool ScreenShareSession::setCameraSendingEnabled(bool enabled) {
    if (enabled) {
        debugCameraRelayTrace(QStringLiteral("[camera-relay] setCameraSendingEnabled enter"));
        if (!m_running.load(std::memory_order_acquire) && !start()) {
            return false;
        }
        const auto enablePlan = VideoSessionStateMachine::planEnableCamera(
            m_cameraSendingEnabled.load(std::memory_order_acquire),
            m_sharingEnabled.load(std::memory_order_acquire));
        if (enablePlan.alreadyEnabled) {
            return true;
        }

        bool startSendThread = false;
        {
            QMutexLocker locker(&m_mutex);
            debugCameraRelayTrace(QStringLiteral("[camera-relay] setCameraSendingEnabled startCameraCaptureLocked"));
            if (!startCameraCaptureLocked()) {
                return false;
            }
            debugCameraRelayTrace(QStringLiteral("[camera-relay] setCameraSendingEnabled startCameraCaptureLocked ok"));
            if (enablePlan.shouldStartSendThread) {
                resetSenderForFreshStream(m_sender,
                                          m_rtcpActionPipeline,
                                          chooseConfiguredOrRandomSsrc(
                                              m_configuredVideoSsrc.load(std::memory_order_acquire)));
                startSendThread = true;
            }
        }

        m_cameraSendingEnabled.store(true, std::memory_order_release);
        if (m_cameraRelay) {
            m_cameraRelay->setCameraEnabled(true);
            m_cameraRelay->setSharingEnabled(m_sharingEnabled.load(std::memory_order_acquire));
        }
        if (enablePlan.shouldRequestKeyFrame) {
            m_forceKeyFramePending.store(true, std::memory_order_release);
        }
        if (startSendThread) {
            debugCameraRelayTrace(QStringLiteral("[camera-relay] setCameraSendingEnabled start sendLoop thread"));
            resetSendThreadStats(m_keyframeRequestCount,
                                 m_retransmitPacketCount,
                                 m_bitrateReconfigureCount,
                                 m_targetBitrateBps,
                                 m_appliedBitrateBps,
                                 m_lastBitrateApplyDelayMs,
                                 m_targetBitrateUpdatedAtMs,
                                 static_cast<uint32_t>(m_config.bitrate),
                                 steadyNowMs());
            const auto sendStartPlan = VideoThreadLifecycleStateMachine::planSendThreadStart(
                startSendThread, m_threadRuntime.sendJoinable());
            if (sendStartPlan.shouldStartSendThread) {
                m_sendFrameRingBuffer.reset();
                m_threadRuntime.startCapture([this] { captureLoop(); },
                                             sendStartPlan.shouldJoinExistingSendThread);
                m_threadRuntime.startSend([this] { sendLoop(); },
                                          sendStartPlan.shouldJoinExistingSendThread);
            }
        }
        debugCameraRelayTrace(QStringLiteral("[camera-relay] setCameraSendingEnabled exit"));
        qInfo().noquote() << "[screen-session] camera sending enabled localPort=" << localPort()
                          << "ssrc=" << videoSsrc();
        m_stateWaitCondition.wakeAll();
        return true;
    }

    if (!m_cameraSendingEnabled.exchange(false, std::memory_order_acq_rel)) {
        return true;
    }
    if (m_cameraRelay) {
        m_cameraRelay->setCameraEnabled(false);
    }

    const auto disablePlan = VideoSessionStateMachine::planDisableCamera(
        true,
        m_sharingEnabled.load(std::memory_order_acquire));
    const auto sendStopPlan = VideoThreadLifecycleStateMachine::planSendThreadStop(
        disablePlan.shouldJoinSendThread, m_threadRuntime.sendJoinable());
    if (sendStopPlan.shouldJoinSendThread) {
        m_sendFrameRingBuffer.close();
        m_threadRuntime.joinCapture();
        m_threadRuntime.joinSend();
    } else if (m_threadRuntime.captureJoinable()) {
        m_sendFrameRingBuffer.close();
        m_threadRuntime.joinCapture();
    }
    if (disablePlan.shouldSetForceKeyFrame) {
        m_forceKeyFramePending.store(disablePlan.forceKeyFrameValue, std::memory_order_release);
    }

    {
        QMutexLocker locker(&m_mutex);
        stopCameraCaptureLocked();
    }
    m_stateWaitCondition.wakeAll();
    return true;
}

bool ScreenShareSession::cameraSendingEnabled() const {
    return m_cameraSendingEnabled.load(std::memory_order_acquire);
}

bool ScreenShareSession::setPreferredCameraDeviceName(const std::string& deviceName) {
    const QString normalizedDeviceName = normalizeCameraDeviceName(QString::fromStdString(deviceName));
    std::function<void(std::string)> statusCallback;
    QString statusMessage;
    {
        QMutexLocker locker(&m_mutex);
        const std::string requestedDeviceName = normalizedDeviceName.toStdString();
        if (m_preferredCameraDeviceName == requestedDeviceName) {
            return true;
        }

        m_preferredCameraDeviceName = requestedDeviceName;
        statusCallback = m_statusCallback;
        if (!m_cameraCapture || !m_cameraCapture->isRunning()) {
            return true;
        }

        const QCameraDevice requestedDevice = resolvePreferredCameraDeviceName(normalizedDeviceName);
        const bool switched = requestedDevice.isNull()
            ? m_cameraCapture->setDeviceSelection(normalizedDeviceName)
            : m_cameraCapture->setDevice(requestedDevice);
        if (!switched) {
            setErrorLocked(QStringLiteral("camera device switch failed: %1").arg(m_cameraCapture->lastError()).toStdString());
            return false;
        }

        const QString backendName = m_cameraCapture->backendName();
        if (!backendName.isEmpty() && backendName != QStringLiteral("qt") && !normalizedDeviceName.isEmpty()) {
            statusMessage = QStringLiteral("Video camera switched to %1 (%2 fallback)").arg(normalizedDeviceName, backendName);
        } else if (!requestedDevice.isNull()) {
            statusMessage = QStringLiteral("Video camera switched to %1").arg(cameraDeviceLabel(requestedDevice));
        } else if (normalizedDeviceName.isEmpty()) {
            statusMessage = QStringLiteral("Video camera switched to system default");
        } else {
            statusMessage = QStringLiteral("Preferred camera unavailable, using system default");
        }
    }

    if (statusCallback && !statusMessage.isEmpty()) {
        statusCallback(statusMessage.toStdString());
    }
    return true;
}

std::string ScreenShareSession::preferredCameraDeviceName() const {
    QMutexLocker locker(&m_mutex);
    return m_preferredCameraDeviceName;
}

bool ScreenShareSession::startCaptureLocked() {
    if (m_capture && m_capture->isRunning()) {
        return true;
    }

    av::capture::ScreenCaptureConfig captureConfig{};
    captureConfig.targetWidth = m_config.width;
    captureConfig.targetHeight = m_config.height;
    captureConfig.frameRate = m_config.frameRate;
    captureConfig.ringCapacity = 3;

    auto capture = std::make_shared<av::capture::ScreenCapture>(captureConfig);
    if (!capture->start()) {
        setErrorLocked("screen capture start failed");
        return false;
    }

    m_capture = std::move(capture);
    return true;
}

void ScreenShareSession::stopCaptureLocked() {
    if (m_capture) {
        m_capture->stop();
        m_capture.reset();
    }
}

bool ScreenShareSession::startCameraCaptureLocked() {
    debugCameraRelayTrace(QStringLiteral("[camera-relay] startCameraCaptureLocked enter"));
    if (!m_cameraRelay) {
        m_cameraRelay = std::make_shared<CameraFrameRelay>(m_config.width, m_config.height, m_config.frameRate);
    }
    if (m_cameraCapture && m_cameraCapture->isRunning()) {
        return true;
    }
    if (m_cameraFallbackCapture && m_cameraFallbackCapture->isRunning()) {
        return true;
    }

    const auto startSyntheticFallback = [this]() {
        av::capture::ScreenCaptureConfig fallbackConfig{};
        fallbackConfig.targetWidth = m_config.width;
        fallbackConfig.targetHeight = m_config.height;
        fallbackConfig.frameRate = m_config.frameRate;
        fallbackConfig.ringCapacity = 3;
        auto fallbackCapture = std::make_shared<av::capture::ScreenCapture>(fallbackConfig);
        if (!fallbackCapture->start()) {
            setErrorLocked("camera fallback capture start failed");
            return false;
        }

        m_cameraCapture.reset();
        m_cameraFallbackCapture = std::move(fallbackCapture);
        qInfo().noquote() << "[screen-session] camera fallback synthetic source enabled";
        if (m_cameraSourceCallback) {
            m_cameraSourceCallback(true);
        }
        return true;
    };

    if (allowSyntheticCameraFallback()) {
        m_cameraRelay->invalidate();
        return startSyntheticFallback();
    }

    const uint64_t generation = m_cameraRelay->beginCapture();
    const QString preferredCameraDevice = QString::fromStdString(m_preferredCameraDeviceName);
    const QCameraDevice requestedDevice = resolvePreferredCameraDeviceName(preferredCameraDevice);
    const QStringList availableCameraDevices = av::capture::CameraCapture::availableDeviceNames();
    const QString requestedCameraLabel = preferredCameraDevice.trimmed().isEmpty()
        ? QStringLiteral("<default>")
        : preferredCameraDevice.trimmed();
    const QString resolvedCameraLabel = !requestedDevice.isNull()
        ? cameraDeviceLabel(requestedDevice)
        : (!preferredCameraDevice.trimmed().isEmpty()
               ? preferredCameraDevice.trimmed()
               : (availableCameraDevices.isEmpty() ? QStringLiteral("<default>") : availableCameraDevices.front()));
    auto cameraCapture = requestedDevice.isNull()
                             ? (preferredCameraDevice.trimmed().isEmpty()
                                    ? std::make_unique<av::capture::CameraCapture>()
                                    : std::make_unique<av::capture::CameraCapture>(preferredCameraDevice.trimmed()))
                             : std::make_unique<av::capture::CameraCapture>(requestedDevice);
    std::weak_ptr<CameraFrameRelay> weakRelay = m_cameraRelay;
    const auto statusCallback = m_statusCallback;
    const auto cameraSourceCallback = m_cameraSourceCallback;
    const auto localCameraPreviewCallback = m_localCameraPreviewCallback;
    const int previewWidth = m_config.width;
    const int previewHeight = m_config.height;
    auto firstCameraFrameObserved = std::make_shared<std::atomic<bool>>(false);
    auto firstCameraFrameConvertFailureReported = std::make_shared<std::atomic<bool>>(false);
    cameraCapture->setFrameCallback([weakRelay,
                                     generation,
                                     statusCallback,
                                     cameraSourceCallback,
                                     localCameraPreviewCallback,
                                     previewWidth,
                                     previewHeight,
                                     firstCameraFrameObserved,
                                     firstCameraFrameConvertFailureReported](av::capture::CameraCaptureFrame frame) {
        if (const auto relay = weakRelay.lock()) {
            QString convertFailureDetail;
            if (frame.hasRawBgra()) {
                convertFailureDetail = QStringLiteral("raw_bgra size=%1x%2 stride=%3")
                                           .arg(frame.width)
                                           .arg(frame.height)
                                           .arg(frame.stride);
            } else {
                const auto format = frame.videoFrame.surfaceFormat();
                const QSize frameSize = format.frameSize();
                convertFailureDetail = QStringLiteral("pixel_format=%1 size=%2x%3")
                                           .arg(static_cast<int>(format.pixelFormat()))
                                           .arg(frameSize.width())
                                           .arg(frameSize.height());
            }
            if (debugCameraRelayEnabled()) {
                qInfo().noquote() << "[camera-relay] callback"
                                  << (frame.hasRawBgra() ? "raw" : "qt")
                                  << convertFailureDetail;
            }
            av::capture::CameraCaptureFrame previewFrame;
            if (localCameraPreviewCallback) {
                previewFrame = frame;
            }
            CameraFrameRelay::EnqueueDropReason dropReason = CameraFrameRelay::EnqueueDropReason::None;
            if (relay->enqueueFrame(generation, std::move(frame), &dropReason)) {
                if (localCameraPreviewCallback) {
                    av::codec::DecodedVideoFrame preview;
                    if (buildLocalCameraPreviewFrame(previewFrame, previewWidth, previewHeight, preview)) {
                        localCameraPreviewCallback(std::move(preview));
                    }
                }
                if (!firstCameraFrameObserved->exchange(true, std::memory_order_acq_rel)) {
                    if (cameraSourceCallback) {
                        cameraSourceCallback(false);
                    }
                    if (statusCallback) {
                        statusCallback("Video camera frame observed");
                    }
                }
                return;
            }

            if (dropReason == CameraFrameRelay::EnqueueDropReason::ConvertFailed &&
                statusCallback &&
                !firstCameraFrameConvertFailureReported->exchange(true, std::memory_order_acq_rel)) {
                statusCallback(QStringLiteral("Video camera frame convert failed: %1").arg(convertFailureDetail).toStdString());
            }
        }
    });
    debugCameraRelayTrace(QStringLiteral("[camera-relay] startCameraCaptureLocked cameraCapture->start begin"));
    if (cameraCapture->start()) {
        const QString activeBackendName = cameraCapture->backendName();
        debugCameraRelayTrace(QStringLiteral("[camera-relay] startCameraCaptureLocked cameraCapture->start ok"));
        m_cameraFallbackCapture.reset();
        m_cameraCapture = std::move(cameraCapture);
        if (m_statusCallback) {
            if (!activeBackendName.isEmpty() && activeBackendName != QStringLiteral("qt") && !preferredCameraDevice.trimmed().isEmpty()) {
                m_statusCallback(QStringLiteral("Video camera device: %1 (%2 fallback)")
                                     .arg(preferredCameraDevice.trimmed(), activeBackendName)
                                     .toStdString());
            } else if (!requestedDevice.isNull()) {
                m_statusCallback(QStringLiteral("Video camera device: %1").arg(requestedDevice.description()).toStdString());
            } else if (!activeBackendName.isEmpty() && activeBackendName != QStringLiteral("qt") && !availableCameraDevices.isEmpty()) {
                m_statusCallback(QStringLiteral("Video camera device: %1 (%2 fallback)")
                                     .arg(availableCameraDevices.front(), activeBackendName)
                                     .toStdString());
            } else if (!preferredCameraDevice.trimmed().isEmpty()) {
                m_statusCallback(QStringLiteral("Preferred camera unavailable, using system default").toStdString());
            }
        }
        return true;
    }
    debugCameraRelayTrace(QStringLiteral("[camera-relay] startCameraCaptureLocked cameraCapture->start failed"));

    cameraCapture->setFrameCallback({});
    m_cameraRelay->invalidate();
    const QString availableCameraLabel = availableCameraDevices.isEmpty()
        ? QStringLiteral("<none>")
        : availableCameraDevices.join(QStringLiteral("|"));
    const QString backendError = cameraCapture->lastError().trimmed().isEmpty()
        ? QStringLiteral("<none>")
        : cameraCapture->lastError().trimmed();
    setErrorLocked(QStringLiteral("camera capture start failed; requested=%1 resolved=%2 available=%3 backend_error=%4")
                       .arg(requestedCameraLabel)
                       .arg(resolvedCameraLabel)
                       .arg(availableCameraLabel)
                       .arg(backendError)
                       .toStdString());
    return false;
}

void ScreenShareSession::stopCameraFallbackCaptureLocked() {
    if (m_cameraFallbackCapture) {
        m_cameraFallbackCapture->stop();
        m_cameraFallbackCapture.reset();
    }
}

void ScreenShareSession::stopCameraCaptureLocked() {
    if (m_cameraRelay) {
        m_cameraRelay->invalidate();
    }
    if (m_cameraCapture) {
        m_cameraCapture->setFrameCallback({});
        m_cameraCapture->stop();
        m_cameraCapture.reset();
    }
    stopCameraFallbackCaptureLocked();
}

}  // namespace av::session
