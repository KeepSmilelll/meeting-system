#include "ScreenCapture.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QScreen>
#include <QThread>
#include <QtGlobal>

#if !defined(_WIN32) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QScreenCapture>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace av::capture {
namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
constexpr int kDefaultFrameRate = 30;

bool allowSyntheticCapture() {
    return qEnvironmentVariableIntValue("MEETING_SYNTHETIC_SCREEN") != 0;
}

ScreenCaptureConfig normalizeConfig(ScreenCaptureConfig config) {
    if (config.targetWidth <= 0) {
        config.targetWidth = kDefaultWidth;
    }
    if (config.targetHeight <= 0) {
        config.targetHeight = kDefaultHeight;
    }
    if (config.frameRate <= 0) {
        config.frameRate = kDefaultFrameRate;
    }
    if (config.ringCapacity == 0) {
        config.ringCapacity = 1;
    }
    return config;
}

bool isApplicationThread() {
    return QCoreApplication::instance() != nullptr &&
           QThread::currentThread() == QCoreApplication::instance()->thread();
}

bool invokeBoolOnApplicationThread(const std::function<bool()>& callback) {
    if (!callback || QCoreApplication::instance() == nullptr) {
        return false;
    }
    if (isApplicationThread()) {
        return callback();
    }

    bool result = false;
    QMetaObject::invokeMethod(QCoreApplication::instance(),
                              [&]() {
                                  result = callback();
                              },
                              Qt::BlockingQueuedConnection);
    return result;
}

void invokeVoidOnApplicationThread(const std::function<void()>& callback) {
    if (!callback || QCoreApplication::instance() == nullptr) {
        return;
    }
    if (isApplicationThread()) {
        callback();
        return;
    }

    QMetaObject::invokeMethod(QCoreApplication::instance(),
                              callback,
                              Qt::BlockingQueuedConnection);
}

}  // namespace

struct ScreenCapture::Impl {
    explicit Impl(ScreenCapture* owner, ScreenCaptureConfig captureConfig)
        : owner(owner),
          config(normalizeConfig(captureConfig)) {}

    void startCapture() {
        running = true;
        if (allowSyntheticCapture()) {
            synthetic = true;
            worker = std::thread(&Impl::captureLoop, this);
            return;
        }
#ifdef _WIN32
        if (initDesktopCapture()) {
            worker = std::thread(&Impl::captureLoop, this);
            return;
        }

        running = false;
        return;
#else
        if (startPortalCapture()) {
            return;
        }
        running = false;
        return;
#endif
    }

    void stopCapture() {
        running = false;
        if (worker.joinable()) {
            worker.join();
        }
#ifdef _WIN32
        releaseDesktopCapture();
#else
        stopPortalCapture();
#endif
    }

    void captureLoop() {
        using namespace std::chrono;
        const auto frameInterval = milliseconds(std::max(1, 1000 / config.frameRate));
        const auto captureStartedAt = steady_clock::now();
        int64_t lastAssignedPts = -1;

        while (running) {
            const auto frameStart = steady_clock::now();
            const auto elapsedSinceStartMs =
                duration_cast<milliseconds>(frameStart - captureStartedAt).count();
            int64_t framePts = static_cast<int64_t>((elapsedSinceStartMs * config.frameRate) / 1000);
            if (framePts <= lastAssignedPts) {
                framePts = lastAssignedPts + 1;
            }

            ScreenFrame frame;
            if (synthetic) {
                frame = makeSyntheticFrame(framePts);
            } else {
#ifdef _WIN32
                frame = captureDesktopFrame(framePts);
#endif
            }

            if (!frame.bgra.empty()) {
                owner->m_ringBuffer.push(std::move(frame));
                if (!loggedFirstFrame) {
                    loggedFirstFrame = true;
                    qInfo().noquote() << "[screen-capture] first frame pts=" << framePts
                                      << "size=" << config.targetWidth << "x" << config.targetHeight;
                }
                lastAssignedPts = framePts;
            }

            const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - frameStart);
            if (elapsed < frameInterval) {
                std::this_thread::sleep_for(frameInterval - elapsed);
            }
        }
    }

    ScreenFrame makeSyntheticFrame(int64_t pts) const {
        ScreenFrame frame;
        frame.width = config.targetWidth;
        frame.height = config.targetHeight;
        frame.pts = pts;
        frame.bgra.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4U, 0);

        const int phase = static_cast<int>(pts % 255);
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
                                            static_cast<std::size_t>(x)) * 4U;
                frame.bgra[offset + 0] = static_cast<uint8_t>((x + phase) % 255);
                frame.bgra[offset + 1] = static_cast<uint8_t>((y * 2 + phase) % 255);
                frame.bgra[offset + 2] = static_cast<uint8_t>((x + y + phase * 3) % 255);
                frame.bgra[offset + 3] = 0xFF;
            }
        }
        return frame;
    }

#ifdef _WIN32
    bool initDesktopCapture() {
        sourceX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        sourceY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        sourceWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        sourceHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (sourceWidth <= 0 || sourceHeight <= 0) {
            qWarning().noquote() << "[screen-capture] invalid virtual screen metrics";
            return false;
        }

        targetWidth = config.targetWidth > 0 ? config.targetWidth : sourceWidth;
        targetHeight = config.targetHeight > 0 ? config.targetHeight : sourceHeight;
        if ((targetWidth & 1) != 0) {
            --targetWidth;
        }
        if ((targetHeight & 1) != 0) {
            --targetHeight;
        }
        targetWidth = std::max(2, targetWidth);
        targetHeight = std::max(2, targetHeight);

        desktopDc = GetDC(nullptr);
        if (desktopDc == nullptr) {
            qWarning().noquote() << "[screen-capture] GetDC failed";
            return false;
        }

        captureDc = CreateCompatibleDC(desktopDc);
        if (captureDc == nullptr) {
            qWarning().noquote() << "[screen-capture] CreateCompatibleDC failed";
            releaseDesktopCapture();
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = targetWidth;
        bmi.bmiHeader.biHeight = -targetHeight;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        bitmap = CreateDIBSection(desktopDc, &bmi, DIB_RGB_COLORS, &bitmapBits, nullptr, 0);
        if (bitmap == nullptr || bitmapBits == nullptr) {
            qWarning().noquote() << "[screen-capture] CreateDIBSection failed";
            releaseDesktopCapture();
            return false;
        }

        previousBitmap = SelectObject(captureDc, bitmap);
        if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
            qWarning().noquote() << "[screen-capture] SelectObject failed";
            releaseDesktopCapture();
            return false;
        }

        SetStretchBltMode(captureDc, HALFTONE);
        SetBrushOrgEx(captureDc, 0, 0, nullptr);
        return true;
    }

    void releaseDesktopCapture() {
        if (captureDc != nullptr && previousBitmap != nullptr && previousBitmap != HGDI_ERROR) {
            SelectObject(captureDc, previousBitmap);
            previousBitmap = nullptr;
        }
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
            bitmap = nullptr;
            bitmapBits = nullptr;
        }
        if (captureDc != nullptr) {
            DeleteDC(captureDc);
            captureDc = nullptr;
        }
        if (desktopDc != nullptr) {
            ReleaseDC(nullptr, desktopDc);
            desktopDc = nullptr;
        }
    }

    ScreenFrame captureDesktopFrame(int64_t pts) const {
        ScreenFrame frame;
        if (desktopDc == nullptr || captureDc == nullptr || bitmapBits == nullptr) {
            return frame;
        }

        if (!StretchBlt(captureDc,
                        0,
                        0,
                        targetWidth,
                        targetHeight,
                        desktopDc,
                        sourceX,
                        sourceY,
                        sourceWidth,
                        sourceHeight,
                        SRCCOPY | CAPTUREBLT)) {
            return frame;
        }

        frame.width = targetWidth;
        frame.height = targetHeight;
        frame.pts = pts;
        const std::size_t bytes = static_cast<std::size_t>(targetWidth) * static_cast<std::size_t>(targetHeight) * 4U;
        frame.bgra.resize(bytes);
        std::memcpy(frame.bgra.data(), bitmapBits, bytes);
        return frame;
    }
#endif

#ifndef _WIN32
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    struct PortalCaptureBackend {
        explicit PortalCaptureBackend(Impl* owner)
            : owner(owner) {}

        bool start() {
            if (owner == nullptr || QGuiApplication::instance() == nullptr) {
                error = QStringLiteral("Qt GUI application instance required for portal screen capture");
                return false;
            }

            QScreen* screen = QGuiApplication::primaryScreen();
            if (screen == nullptr) {
                error = QStringLiteral("no primary screen available for portal screen capture");
                return false;
            }

            screenCapture = std::make_unique<QScreenCapture>();
            videoSink = std::make_unique<QVideoSink>();
            session.setScreenCapture(screenCapture.get());
            session.setVideoSink(videoSink.get());
            screenCapture->setScreen(screen);

            QObject::connect(videoSink.get(),
                             &QVideoSink::videoFrameChanged,
                             videoSink.get(),
                             [this](const QVideoFrame& frame) {
                                 handleFrame(frame);
                             });
            QObject::connect(screenCapture.get(),
                             &QScreenCapture::errorOccurred,
                             screenCapture.get(),
                             [this](QScreenCapture::Error captureError, const QString& errorString) {
                                 if (captureError == QScreenCapture::NoError) {
                                     return;
                                 }
                                 error = errorString.trimmed().isEmpty()
                                     ? QStringLiteral("portal screen capture error %1").arg(static_cast<int>(captureError))
                                     : errorString.trimmed();
                                 if (owner != nullptr) {
                                     owner->running.store(false, std::memory_order_release);
                                     if (owner->owner != nullptr) {
                                         owner->owner->m_running.store(false, std::memory_order_release);
                                         owner->owner->m_ringBuffer.close();
                                     }
                                 }
                                 qWarning().noquote() << "[screen-capture] portal backend error:" << error;
                             });

            screenCapture->start();
            return true;
        }

        void stop() {
            if (screenCapture) {
                screenCapture->stop();
            }
            session.setVideoSink(nullptr);
            session.setScreenCapture(nullptr);
            videoSink.reset();
            screenCapture.reset();
        }

        void handleFrame(const QVideoFrame& videoFrame) {
            if (owner == nullptr || !owner->running.load(std::memory_order_acquire) || !videoFrame.isValid()) {
                return;
            }

            QImage image = videoFrame.toImage();
            if (image.isNull()) {
                return;
            }
            if (image.format() != QImage::Format_ARGB32) {
                image = image.convertToFormat(QImage::Format_ARGB32);
            }
            if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
                return;
            }

            ScreenFrame frame;
            frame.width = image.width() & ~1;
            frame.height = image.height() & ~1;
            if (owner->config.targetWidth > 0 && owner->config.targetHeight > 0 &&
                (frame.width != owner->config.targetWidth || frame.height != owner->config.targetHeight)) {
                QImage scaled = image.scaled(owner->config.targetWidth & ~1,
                                             owner->config.targetHeight & ~1,
                                             Qt::IgnoreAspectRatio,
                                             Qt::SmoothTransformation);
                if (!scaled.isNull()) {
                    if (scaled.format() != QImage::Format_ARGB32) {
                        scaled = scaled.convertToFormat(QImage::Format_ARGB32);
                    }
                    image = std::move(scaled);
                    frame.width = image.width() & ~1;
                    frame.height = image.height() & ~1;
                }
            }
            if (frame.width <= 0 || frame.height <= 0) {
                return;
            }

            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - captureStartedAt)
                    .count();
            int64_t nextPts = static_cast<int64_t>(
                (elapsedMs * static_cast<int64_t>(owner->config.frameRate)) / 1000);
            if (nextPts <= lastPts) {
                nextPts = lastPts + 1;
            }
            lastPts = nextPts;
            frame.pts = nextPts;
            frame.bgra.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4U);
            for (int y = 0; y < frame.height; ++y) {
                const uchar* src = image.constScanLine(y);
                std::memcpy(frame.bgra.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) * 4U,
                            src,
                            static_cast<std::size_t>(frame.width) * 4U);
            }

            owner->m_ownerPushFrame(std::move(frame));
        }

        Impl* owner{nullptr};
        QMediaCaptureSession session;
        std::unique_ptr<QScreenCapture> screenCapture;
        std::unique_ptr<QVideoSink> videoSink;
        QString error;
        std::chrono::steady_clock::time_point captureStartedAt{std::chrono::steady_clock::now()};
        int64_t lastPts{-1};
    };

    bool startPortalCapture() {
        if (QCoreApplication::instance() == nullptr) {
            qWarning().noquote() << "[screen-capture] portal backend unavailable: no Qt application instance";
            return false;
        }

        const bool started = invokeBoolOnApplicationThread([this]() {
            portalCapture = std::make_unique<PortalCaptureBackend>(this);
            if (!portalCapture->start()) {
                qWarning().noquote() << "[screen-capture] portal backend start failed:" << portalCapture->error;
                portalCapture.reset();
                return false;
            }
            return true;
        });
        if (started) {
            qInfo().noquote() << "[screen-capture] portal backend active screenBackend=portal-qt screenInterop=cpu-upload";
        }
        return started;
    }

    void stopPortalCapture() {
        invokeVoidOnApplicationThread([this]() {
            if (portalCapture) {
                portalCapture->stop();
                portalCapture.reset();
            }
        });
    }
#else
    bool startPortalCapture() {
        qWarning().noquote() << "[screen-capture] portal backend unavailable: Qt 6.5+ QScreenCapture required";
        return false;
    }

    void stopPortalCapture() {}
#endif
#endif

    ScreenCapture* owner{nullptr};
    ScreenCaptureConfig config;
    std::thread worker;
    std::atomic<bool> running{false};
    bool synthetic{false};
    bool loggedFirstFrame{false};

#ifdef _WIN32
    HDC desktopDc{nullptr};
    HDC captureDc{nullptr};
    HBITMAP bitmap{nullptr};
    HGDIOBJ previousBitmap{nullptr};
    void* bitmapBits{nullptr};
    int sourceX{0};
    int sourceY{0};
    int sourceWidth{0};
    int sourceHeight{0};
    int targetWidth{0};
    int targetHeight{0};
#else
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    std::unique_ptr<PortalCaptureBackend> portalCapture;
#endif
#endif

    void m_ownerPushFrame(ScreenFrame frame) {
        const int64_t pts = frame.pts;
        const int width = frame.width;
        const int height = frame.height;
        owner->m_ringBuffer.push(std::move(frame));
        if (!loggedFirstFrame) {
            loggedFirstFrame = true;
            qInfo().noquote() << "[screen-capture] first frame pts=" << pts
                              << "size=" << width << "x" << height
                              << "screenBackend=portal-qt"
                              << "screenInterop=cpu-upload";
        }
    }
};

ScreenCapture::ScreenCapture(ScreenCaptureConfig config)
    : m_config(normalizeConfig(std::move(config))),
      m_ringBuffer(m_config.ringCapacity) {}

ScreenCapture::~ScreenCapture() {
    stop();
}

bool ScreenCapture::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        return false;
    }

    m_impl = std::make_unique<Impl>(this, m_config);
    m_impl->startCapture();
    if (!m_impl->running) {
        m_impl.reset();
        m_running.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void ScreenCapture::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel) && !m_impl) {
        return;
    }

    if (m_impl) {
        m_impl->stopCapture();
        m_impl.reset();
    }

    m_ringBuffer.close();
}

bool ScreenCapture::isRunning() const {
    return m_running.load(std::memory_order_acquire);
}

bool ScreenCapture::popFrameForEncode(ScreenFrame& outFrame, std::chrono::milliseconds timeout) {
    if (!isRunning()) {
        return false;
    }
    return m_ringBuffer.popWait(outFrame, timeout);
}

}  // namespace av::capture
