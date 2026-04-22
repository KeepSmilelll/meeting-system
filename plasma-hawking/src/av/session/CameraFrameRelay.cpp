#include "CameraFrameRelay.h"

#include <QDebug>
#include <QImage>
#include <QMutexLocker>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoFrameFormat>
#include <QtGlobal>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

namespace av::session {
namespace {

bool debugCameraRelayEnabled() {
    return qEnvironmentVariableIntValue("MEETING_DEBUG_CAMERA_RELAY") != 0;
}

bool copyPlaneRows(uint8_t* dst,
                   int dstStride,
                   const uint8_t* src,
                   int srcStride,
                   int rowBytes,
                   int rows) {
    if (dst == nullptr || src == nullptr || rowBytes <= 0 || rows <= 0) {
        return false;
    }
    if (dstStride < rowBytes || srcStride < rowBytes) {
        return false;
    }

    for (int row = 0; row < rows; ++row) {
        std::memcpy(dst + static_cast<std::ptrdiff_t>(row) * dstStride,
                    src + static_cast<std::ptrdiff_t>(row) * srcStride,
                    static_cast<std::size_t>(rowBytes));
    }
    return true;
}

bool buildBgraAvFrameFromMappedData(const uint8_t* bgraData,
                                    int bgraStride,
                                    int width,
                                    int height,
                                    int64_t pts,
                                    av::AVFramePtr& outFrame) {
    if (bgraData == nullptr || width <= 0 || height <= 0 || bgraStride < width * 4) {
        return false;
    }

    av::AVFramePtr frame = av::makeFrame();
    if (!frame) {
        return false;
    }
    frame->format = AV_PIX_FMT_BGRA;
    frame->width = width;
    frame->height = height;
    frame->pts = pts;
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        return false;
    }
    if (!copyPlaneRows(frame->data[0],
                       frame->linesize[0],
                       bgraData,
                       bgraStride,
                       width * 4,
                       height)) {
        return false;
    }

    outFrame = std::move(frame);
    return true;
}

bool buildBgraAvFrameFromImage(QImage image,
                               int targetWidth,
                               int targetHeight,
                               int64_t pts,
                               av::AVFramePtr& outFrame) {
    if (image.isNull()) {
        return false;
    }
    if (image.width() != targetWidth || image.height() != targetHeight) {
        image = image.scaled(targetWidth, targetHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_ARGB32);
    }
    if (image.isNull()) {
        return false;
    }
    return buildBgraAvFrameFromMappedData(
        image.constBits(),
        image.bytesPerLine(),
        targetWidth,
        targetHeight,
        pts,
        outFrame);
}

bool tryBuildAvFrameFromQVideoFrame(const QVideoFrame& inputFrame,
                                    int targetWidth,
                                    int targetHeight,
                                    int64_t pts,
                                    av::AVFramePtr& outFrame) {
    QVideoFrame sourceFrame(inputFrame);
    if (!sourceFrame.isValid()) {
        return false;
    }

    const auto surfaceFormat = sourceFrame.surfaceFormat();
    const QSize frameSize = surfaceFormat.frameSize();
    if (frameSize.width() != targetWidth || frameSize.height() != targetHeight) {
        return false;
    }

    AVPixelFormat outputPixelFormat = AV_PIX_FMT_NONE;
    switch (surfaceFormat.pixelFormat()) {
    case QVideoFrameFormat::Format_NV12:
        outputPixelFormat = AV_PIX_FMT_NV12;
        break;
    case QVideoFrameFormat::Format_YUV420P:
        outputPixelFormat = AV_PIX_FMT_YUV420P;
        break;
    case QVideoFrameFormat::Format_BGRA8888:
        outputPixelFormat = AV_PIX_FMT_BGRA;
        break;
    default:
        return false;
    }

    if (!sourceFrame.map(QVideoFrame::ReadOnly)) {
        return false;
    }
    struct FrameUnmapper {
        QVideoFrame& frame;
        ~FrameUnmapper() {
            frame.unmap();
        }
    } frameUnmapper{sourceFrame};

    av::AVFramePtr frame = av::makeFrame();
    if (!frame) {
        return false;
    }
    frame->format = static_cast<int>(outputPixelFormat);
    frame->width = targetWidth;
    frame->height = targetHeight;
    frame->pts = pts;
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        return false;
    }

    const uint8_t* yPlane = sourceFrame.bits(0);
    const int yStride = sourceFrame.bytesPerLine(0);
    if (!copyPlaneRows(frame->data[0], frame->linesize[0], yPlane, yStride, targetWidth, targetHeight)) {
        return false;
    }

    if (outputPixelFormat == AV_PIX_FMT_BGRA) {
        const uint8_t* bgraPlane = sourceFrame.bits(0);
        const int bgraStride = sourceFrame.bytesPerLine(0);
        if (!copyPlaneRows(frame->data[0],
                           frame->linesize[0],
                           bgraPlane,
                           bgraStride,
                           targetWidth * 4,
                           targetHeight)) {
            return false;
        }
    } else if (outputPixelFormat == AV_PIX_FMT_NV12) {
        const int chromaHeight = targetHeight / 2;
        const uint8_t* uvPlane = sourceFrame.bits(1);
        const int uvStride = sourceFrame.bytesPerLine(1);
        if (!copyPlaneRows(frame->data[1], frame->linesize[1], uvPlane, uvStride, targetWidth, chromaHeight)) {
            return false;
        }
    } else if (outputPixelFormat == AV_PIX_FMT_YUV420P) {
        const int chromaHeight = targetHeight / 2;
        const int chromaWidth = targetWidth / 2;
        const uint8_t* uPlane = sourceFrame.bits(1);
        const uint8_t* vPlane = sourceFrame.bits(2);
        const int uStride = sourceFrame.bytesPerLine(1);
        const int vStride = sourceFrame.bytesPerLine(2);
        if (!copyPlaneRows(frame->data[1], frame->linesize[1], uPlane, uStride, chromaWidth, chromaHeight)) {
            return false;
        }
        if (!copyPlaneRows(frame->data[2], frame->linesize[2], vPlane, vStride, chromaWidth, chromaHeight)) {
            return false;
        }
    } else {
        return false;
    }

    outFrame = std::move(frame);
    return true;
}

}  // namespace

CameraFrameRelay::CameraFrameRelay(int targetWidth, int targetHeight, int targetFrameRate)
    : m_width(std::max(2, targetWidth & ~1)),
      m_height(std::max(2, targetHeight & ~1)),
      m_targetFrameRate(std::max(1, targetFrameRate)),
      m_frameInterval(std::chrono::milliseconds(std::max(1, 1000 / std::max(1, targetFrameRate)))) {}

uint64_t CameraFrameRelay::beginCapture() {
    const uint64_t nextGeneration = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        QMutexLocker locker(&m_mutex);
        m_frames.clear();
        m_lastAcceptedAt = std::chrono::steady_clock::time_point{};
        m_ptsOriginAt = std::chrono::steady_clock::time_point{};
        m_lastAssignedPts = -1;
    }
    m_cv.wakeAll();
    return nextGeneration;
}

void CameraFrameRelay::invalidate() {
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    setCameraEnabled(false);
}

void CameraFrameRelay::setCameraEnabled(bool enabled) {
    m_cameraEnabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        QMutexLocker locker(&m_mutex);
        m_frames.clear();
        m_lastAcceptedAt = std::chrono::steady_clock::time_point{};
        m_ptsOriginAt = std::chrono::steady_clock::time_point{};
        m_lastAssignedPts = -1;
    }
    m_cv.wakeAll();
}

void CameraFrameRelay::setSharingEnabled(bool enabled) {
    m_sharingEnabled.store(enabled, std::memory_order_release);
    m_cv.wakeAll();
}

bool CameraFrameRelay::popFrame(av::capture::ScreenFrame& outFrame, std::chrono::milliseconds timeout) {
    QueuedCameraFrame queued;
    {
        QMutexLocker locker(&m_mutex);
        const auto timeoutCount = timeout.count() > 0 ? timeout.count() : 0;
        const auto timeoutMs = std::chrono::milliseconds(timeoutCount);
        const auto deadline = std::chrono::steady_clock::now() + timeoutMs;
        while (m_frames.empty() &&
               m_cameraEnabled.load(std::memory_order_acquire) &&
               !m_sharingEnabled.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto waitCount = remaining.count() > 1 ? remaining.count() : 1;
            const auto waitMs = static_cast<unsigned long>(waitCount);
            if (!m_cv.wait(&m_mutex, waitMs)) {
                return false;
            }
        }
        if (m_frames.empty()) {
            return false;
        }

        queued = std::move(m_frames.front());
        m_frames.pop_front();
    }

    outFrame.width = m_width;
    outFrame.height = m_height;
    outFrame.pts = queued.pts;
    const std::size_t lineBytes = static_cast<std::size_t>(m_width) * 4U;
    if (queued.frame.hasRawBgra()) {
        if (debugCameraRelayEnabled()) {
            qInfo().noquote() << "[camera-relay] pop raw"
                              << "src=" << queued.frame.width << "x" << queued.frame.height
                              << "stride=" << queued.frame.stride
                              << "bytes=" << queued.frame.bgra.size()
                              << "pts=" << queued.pts;
        }
        const int sourceWidth = queued.frame.width;
        const int sourceHeight = queued.frame.height;
        const int sourceStride = queued.frame.stride;
        if (sourceWidth <= 0 || sourceHeight <= 0 || sourceStride < sourceWidth * 4) {
            return false;
        }

        if (sourceWidth == m_width &&
            sourceHeight == m_height &&
            sourceStride == static_cast<int>(lineBytes) &&
            queued.frame.bgra.size() == lineBytes * static_cast<std::size_t>(m_height)) {
            outFrame.bgra = std::move(queued.frame.bgra);
            return true;
        }

        QImage bgra(reinterpret_cast<const uchar*>(queued.frame.bgra.data()),
                    sourceWidth,
                    sourceHeight,
                    sourceStride,
                    QImage::Format_ARGB32);
        if (bgra.isNull()) {
            return false;
        }
        if (bgra.width() != m_width || bgra.height() != m_height) {
            bgra = bgra.scaled(m_width, m_height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        } else if (sourceStride != static_cast<int>(lineBytes)) {
            bgra = bgra.copy();
        }
        if (bgra.format() != QImage::Format_ARGB32) {
            bgra = bgra.convertToFormat(QImage::Format_ARGB32);
        }
        if (bgra.isNull()) {
            return false;
        }

        outFrame.bgra.resize(lineBytes * static_cast<std::size_t>(m_height));
        for (int y = 0; y < m_height; ++y) {
            std::memcpy(outFrame.bgra.data() + static_cast<std::size_t>(y) * lineBytes,
                        bgra.constScanLine(y),
                        lineBytes);
        }
        return true;
    }

    if (debugCameraRelayEnabled()) {
        const auto format = queued.frame.videoFrame.surfaceFormat();
        qInfo().noquote() << "[camera-relay] pop qt"
                          << "pixel_format=" << static_cast<int>(format.pixelFormat())
                          << "size=" << format.frameSize()
                          << "pts=" << queued.pts;
    }
    QVideoFrame cpuFrame(queued.frame.videoFrame);
    QImage bgra = cpuFrame.toImage();
    if (bgra.isNull() && cpuFrame.map(QVideoFrame::ReadOnly)) {
        bgra = cpuFrame.toImage();
        cpuFrame.unmap();
    }
    if (bgra.isNull()) {
        return false;
    }
    if (bgra.format() != QImage::Format_ARGB32) {
        bgra = bgra.convertToFormat(QImage::Format_ARGB32);
    }
    if (bgra.isNull()) {
        return false;
    }
    if (bgra.width() != m_width || bgra.height() != m_height) {
        bgra = bgra.scaled(m_width, m_height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (bgra.format() != QImage::Format_ARGB32) {
            bgra = bgra.convertToFormat(QImage::Format_ARGB32);
        }
    }
    if (bgra.isNull()) {
        return false;
    }

    outFrame.bgra.resize(lineBytes * static_cast<std::size_t>(m_height));
    for (int y = 0; y < m_height; ++y) {
        std::memcpy(outFrame.bgra.data() + static_cast<std::size_t>(y) * lineBytes,
                    bgra.constScanLine(y),
                    lineBytes);
    }
    return true;
}

bool CameraFrameRelay::popFrameForEncode(CameraFrameForEncode& outFrame, std::chrono::milliseconds timeout) {
    QueuedCameraFrame queued;
    {
        QMutexLocker locker(&m_mutex);
        const auto timeoutCount = timeout.count() > 0 ? timeout.count() : 0;
        const auto timeoutMs = std::chrono::milliseconds(timeoutCount);
        const auto deadline = std::chrono::steady_clock::now() + timeoutMs;
        while (m_frames.empty() &&
               m_cameraEnabled.load(std::memory_order_acquire) &&
               !m_sharingEnabled.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto waitCount = remaining.count() > 1 ? remaining.count() : 1;
            const auto waitMs = static_cast<unsigned long>(waitCount);
            if (!m_cv.wait(&m_mutex, waitMs)) {
                return false;
            }
        }
        if (m_frames.empty()) {
            return false;
        }

        queued = std::move(m_frames.front());
        m_frames.pop_front();
    }

    outFrame = CameraFrameForEncode{};
    outFrame.pts = queued.pts;
    const std::size_t lineBytes = static_cast<std::size_t>(m_width) * 4U;
    if (queued.frame.hasRawBgra()) {
        if (debugCameraRelayEnabled()) {
            qInfo().noquote() << "[camera-relay] pop raw"
                              << "src=" << queued.frame.width << "x" << queued.frame.height
                              << "stride=" << queued.frame.stride
                              << "bytes=" << queued.frame.bgra.size()
                              << "pts=" << queued.pts;
        }
        const int sourceWidth = queued.frame.width;
        const int sourceHeight = queued.frame.height;
        const int sourceStride = queued.frame.stride;
        if (sourceWidth <= 0 || sourceHeight <= 0 || sourceStride < sourceWidth * 4) {
            return false;
        }

        if (sourceWidth == m_width &&
            sourceHeight == m_height &&
            sourceStride <= static_cast<int>(queued.frame.bgra.size() / static_cast<std::size_t>(m_height)) &&
            buildBgraAvFrameFromMappedData(
                queued.frame.bgra.data(),
                sourceStride,
                m_width,
                m_height,
                queued.pts,
                outFrame.avFrame)) {
            return true;
        }

        QImage bgra(reinterpret_cast<const uchar*>(queued.frame.bgra.data()),
                    sourceWidth,
                    sourceHeight,
                    sourceStride,
                    QImage::Format_ARGB32);
        if (buildBgraAvFrameFromImage(
                std::move(bgra),
                m_width,
                m_height,
                queued.pts,
                outFrame.avFrame)) {
            return true;
        }

        outFrame.screenFrame.width = m_width;
        outFrame.screenFrame.height = m_height;
        outFrame.screenFrame.pts = queued.pts;
        outFrame.screenFrame.bgra.resize(lineBytes * static_cast<std::size_t>(m_height));
        if (sourceWidth == m_width &&
            sourceHeight == m_height &&
            sourceStride <= static_cast<int>(queued.frame.bgra.size() / static_cast<std::size_t>(m_height))) {
            for (int y = 0; y < m_height; ++y) {
                const uint8_t* src = queued.frame.bgra.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(sourceStride);
                std::memcpy(outFrame.screenFrame.bgra.data() + static_cast<std::size_t>(y) * lineBytes,
                            src,
                            lineBytes);
            }
            return true;
        }
        return false;
    }

    if (debugCameraRelayEnabled()) {
        const auto format = queued.frame.videoFrame.surfaceFormat();
        qInfo().noquote() << "[camera-relay] pop qt"
                          << "pixel_format=" << static_cast<int>(format.pixelFormat())
                          << "size=" << format.frameSize()
                          << "pts=" << queued.pts;
    }
    if (tryBuildAvFrameFromQVideoFrame(
            queued.frame.videoFrame, m_width, m_height, queued.pts, outFrame.avFrame)) {
        return true;
    }

    QVideoFrame cpuFrame(queued.frame.videoFrame);
    QImage bgra = cpuFrame.toImage();
    if (bgra.isNull() && cpuFrame.map(QVideoFrame::ReadOnly)) {
        bgra = cpuFrame.toImage();
        cpuFrame.unmap();
    }
    if (buildBgraAvFrameFromImage(
            std::move(bgra),
            m_width,
            m_height,
            queued.pts,
            outFrame.avFrame)) {
        return true;
    }

    outFrame.screenFrame.width = m_width;
    outFrame.screenFrame.height = m_height;
    outFrame.screenFrame.pts = queued.pts;
    return false;
}

bool CameraFrameRelay::enqueueFrame(uint64_t expectedGeneration,
                                    av::capture::CameraCaptureFrame frame,
                                    EnqueueDropReason* dropReason) {
    const auto setDropReason = [dropReason](EnqueueDropReason reason) {
        if (dropReason != nullptr) {
            *dropReason = reason;
        }
    };

    if (!frame.isValid() ||
        !m_cameraEnabled.load(std::memory_order_acquire) ||
        m_sharingEnabled.load(std::memory_order_acquire) ||
        m_generation.load(std::memory_order_acquire) != expectedGeneration) {
        if (!frame.isValid()) {
            setDropReason(EnqueueDropReason::InvalidFrame);
        } else if (m_generation.load(std::memory_order_acquire) != expectedGeneration) {
            setDropReason(EnqueueDropReason::GenerationMismatch);
        } else {
            setDropReason(EnqueueDropReason::DisabledOrSharing);
        }
        return false;
    }

    {
        QMutexLocker locker(&m_mutex);
        if (!m_cameraEnabled.load(std::memory_order_acquire) ||
            m_sharingEnabled.load(std::memory_order_acquire) ||
            m_generation.load(std::memory_order_acquire) != expectedGeneration) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_lastAcceptedAt != std::chrono::steady_clock::time_point{} &&
            now - m_lastAcceptedAt < m_frameInterval) {
            setDropReason(EnqueueDropReason::Throttled);
            return false;
        }
        m_lastAcceptedAt = now;
    }

    {
        QMutexLocker locker(&m_mutex);
        if (!m_cameraEnabled.load(std::memory_order_acquire) ||
            m_sharingEnabled.load(std::memory_order_acquire) ||
            m_generation.load(std::memory_order_acquire) != expectedGeneration) {
            if (m_generation.load(std::memory_order_acquire) != expectedGeneration) {
                setDropReason(EnqueueDropReason::GenerationMismatch);
            } else {
                setDropReason(EnqueueDropReason::DisabledOrSharing);
            }
            return false;
        }

        const auto ptsNow = std::chrono::steady_clock::now();
        if (m_ptsOriginAt == std::chrono::steady_clock::time_point{}) {
            m_ptsOriginAt = ptsNow;
        }
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(ptsNow - m_ptsOriginAt).count();
        int64_t computedPts = static_cast<int64_t>((elapsedMs * m_targetFrameRate) / 1000);
        if (computedPts <= m_lastAssignedPts) {
            computedPts = m_lastAssignedPts + 1;
        }
        m_lastAssignedPts = computedPts;
        QueuedCameraFrame queued;
        queued.frame = std::move(frame);
        queued.pts = computedPts;
        if (debugCameraRelayEnabled()) {
            qInfo().noquote() << "[camera-relay] enqueue"
                              << (queued.frame.hasRawBgra() ? "raw" : "qt")
                              << "pts=" << queued.pts
                              << "queue_before=" << m_frames.size();
        }

        if (m_frames.size() >= kCapacity) {
            m_frames.pop_front();
        }
        m_frames.push_back(std::move(queued));
    }
    m_cv.wakeOne();
    setDropReason(EnqueueDropReason::None);
    return true;
}

}  // namespace av::session
