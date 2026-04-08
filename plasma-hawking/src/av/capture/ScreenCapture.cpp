#include "ScreenCapture.h"

#include <QDebug>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
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
constexpr int kDefaultFrameRate = 5;

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
#endif
    }

    void captureLoop() {
        using namespace std::chrono;
        const auto frameInterval = milliseconds(std::max(1, 1000 / config.frameRate));
        int64_t nextPts = 0;

        while (running) {
            const auto frameStart = steady_clock::now();

            ScreenFrame frame;
            if (synthetic) {
                frame = makeSyntheticFrame(nextPts);
            } else {
#ifdef _WIN32
                frame = captureDesktopFrame(nextPts);
#endif
            }

            if (!frame.bgra.empty()) {
                owner->m_ringBuffer.push(std::move(frame));
                if (!loggedFirstFrame) {
                    loggedFirstFrame = true;
                    qInfo().noquote() << "[screen-capture] first frame pts=" << nextPts
                                      << "size=" << config.targetWidth << "x" << config.targetHeight;
                }
                ++nextPts;
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
#endif
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
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
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
