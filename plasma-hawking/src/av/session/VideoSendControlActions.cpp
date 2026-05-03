#include "VideoSendControlActions.h"

#include "av/FFmpegUtils.h"

#include <QImage>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace av::session {
namespace {

constexpr uint32_t kMinAdaptiveBitrateBps = 100000U;

struct SwsContextDeleter {
    void operator()(SwsContext* context) const {
        sws_freeContext(context);
    }
};

using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

int normalizeEvenDimension(int value) {
    value = std::max(2, value);
    value &= ~1;
    return std::max(2, value);
}

void storeApplyDelay(std::atomic<uint64_t>& targetBitrateUpdatedAtMs,
                     std::atomic<uint32_t>& lastBitrateApplyDelayMs,
                     uint64_t nowMs) {
    const uint64_t updatedAtMs = targetBitrateUpdatedAtMs.load(std::memory_order_acquire);
    if (updatedAtMs == 0 || nowMs < updatedAtMs) {
        return;
    }

    const uint64_t delayMs = nowMs - updatedAtMs;
    const uint32_t boundedDelay = delayMs > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(delayMs);
    lastBitrateApplyDelayMs.store(boundedDelay, std::memory_order_release);
}

bool copyQImageToScreenFrame(const QImage& image,
                             int64_t pts,
                             av::capture::ScreenFrame& outFrame) {
    if (image.isNull() || image.format() != QImage::Format_ARGB32) {
        return false;
    }
    const int width = image.width();
    const int height = image.height();
    if (width <= 0 || height <= 0 || (width % 2) != 0 || (height % 2) != 0) {
        return false;
    }

    outFrame = av::capture::ScreenFrame{};
    outFrame.width = width;
    outFrame.height = height;
    outFrame.pts = pts;
    const std::size_t lineBytes = static_cast<std::size_t>(width) * 4U;
    outFrame.bgra.resize(lineBytes * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        std::memcpy(outFrame.bgra.data() + static_cast<std::size_t>(y) * lineBytes,
                    image.constScanLine(y),
                    lineBytes);
    }
    return true;
}

bool scaleScreenFrame(const av::capture::ScreenFrame& inputFrame,
                      int targetWidth,
                      int targetHeight,
                      av::capture::ScreenFrame& outFrame) {
    if (inputFrame.width <= 0 || inputFrame.height <= 0 || inputFrame.bgra.empty()) {
        return false;
    }
    const std::size_t expectedBytes =
        static_cast<std::size_t>(inputFrame.width) * static_cast<std::size_t>(inputFrame.height) * 4U;
    if (inputFrame.bgra.size() != expectedBytes) {
        return false;
    }
    if (inputFrame.width == targetWidth && inputFrame.height == targetHeight) {
        outFrame = inputFrame;
        return true;
    }

    QImage image(reinterpret_cast<const uchar*>(inputFrame.bgra.data()),
                 inputFrame.width,
                 inputFrame.height,
                 inputFrame.width * 4,
                 QImage::Format_ARGB32);
    if (image.isNull()) {
        return false;
    }
    QImage scaled = image.scaled(targetWidth,
                                 targetHeight,
                                 Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
    if (scaled.format() != QImage::Format_ARGB32) {
        scaled = scaled.convertToFormat(QImage::Format_ARGB32);
    }
    return copyQImageToScreenFrame(scaled, inputFrame.pts, outFrame);
}

bool scaleAvFrame(const AVFrame& inputFrame,
                  int targetWidth,
                  int targetHeight,
                  av::AVFramePtr& outFrame) {
    const auto inputFormat = static_cast<AVPixelFormat>(inputFrame.format);
    if (inputFrame.width <= 0 || inputFrame.height <= 0 || inputFormat == AV_PIX_FMT_NONE) {
        return false;
    }
    if (inputFrame.width == targetWidth && inputFrame.height == targetHeight) {
        AVFrame* cloned = av_frame_clone(&inputFrame);
        if (cloned == nullptr) {
            return false;
        }
        outFrame.reset(cloned);
        return true;
    }

    av::AVFramePtr scaled = av::makeFrame();
    if (!scaled) {
        return false;
    }
    scaled->format = static_cast<int>(inputFormat);
    scaled->width = targetWidth;
    scaled->height = targetHeight;
    scaled->pts = inputFrame.pts;
    if (av_frame_get_buffer(scaled.get(), 32) < 0) {
        return false;
    }

    SwsContextPtr context(sws_getContext(inputFrame.width,
                                         inputFrame.height,
                                         inputFormat,
                                         targetWidth,
                                         targetHeight,
                                         inputFormat,
                                         SWS_BILINEAR,
                                         nullptr,
                                         nullptr,
                                         nullptr));
    if (!context) {
        return false;
    }
    const int result = sws_scale(context.get(),
                                 inputFrame.data,
                                 inputFrame.linesize,
                                 0,
                                 inputFrame.height,
                                 scaled->data,
                                 scaled->linesize);
    if (result != targetHeight) {
        return false;
    }

    outFrame = std::move(scaled);
    return true;
}

}  // namespace

bool shouldReportCameraFrameTimeout(CameraFrameTimeoutState& state,
                                    uint64_t nowMs,
                                    uint64_t timeoutMs) {
    if (state.waitStartedAtMs == 0) {
        state.waitStartedAtMs = nowMs;
        return false;
    }
    if (state.timeoutReported) {
        return false;
    }
    if (nowMs - state.waitStartedAtMs < timeoutMs) {
        return false;
    }

    state.timeoutReported = true;
    return true;
}

void resetCameraFrameTimeout(CameraFrameTimeoutState& state) {
    state.timeoutReported = false;
    state.waitStartedAtMs = 0;
}

bool maybeApplyTargetBitrate(av::codec::VideoEncoder& encoder,
                             std::atomic<uint32_t>& targetBitrateBps,
                             std::atomic<uint32_t>& appliedBitrateBps,
                             std::atomic<uint64_t>& bitrateReconfigureCount,
                             std::atomic<uint64_t>& targetBitrateUpdatedAtMs,
                             std::atomic<uint32_t>& lastBitrateApplyDelayMs,
                             uint64_t nowMs,
                             std::string* error) {
    const uint32_t targetBitrate = targetBitrateBps.load(std::memory_order_acquire);
    if (targetBitrate < kMinAdaptiveBitrateBps || static_cast<int>(targetBitrate) == encoder.bitrate()) {
        return true;
    }

    if (!encoder.setBitrate(static_cast<int>(targetBitrate))) {
        if (error) {
            *error = "video encoder bitrate update failed";
        }
        return false;
    }

    appliedBitrateBps.store(static_cast<uint32_t>(encoder.bitrate()), std::memory_order_release);
    bitrateReconfigureCount.fetch_add(1, std::memory_order_acq_rel);
    storeApplyDelay(targetBitrateUpdatedAtMs, lastBitrateApplyDelayMs, nowMs);

    return true;
}

bool maybeApplyAdaptiveEncoderProfile(av::codec::VideoEncoder& encoder,
                                      int targetWidth,
                                      int targetHeight,
                                      int targetFrameRate,
                                      uint32_t targetBitrateBps,
                                      uint8_t payloadType,
                                      av::codec::VideoEncoderPreset preset,
                                      std::atomic<uint32_t>& appliedBitrateBps,
                                      std::atomic<uint64_t>& bitrateReconfigureCount,
                                      std::atomic<uint64_t>& targetBitrateUpdatedAtMs,
                                      std::atomic<uint32_t>& lastBitrateApplyDelayMs,
                                      uint64_t nowMs,
                                      std::string* error) {
    targetWidth = normalizeEvenDimension(targetWidth);
    targetHeight = normalizeEvenDimension(targetHeight);
    targetFrameRate = std::max(1, targetFrameRate);
    const uint32_t boundedBitrate = std::max(kMinAdaptiveBitrateBps, targetBitrateBps);

    if (encoder.width() == targetWidth &&
        encoder.height() == targetHeight &&
        encoder.frameRate() == targetFrameRate) {
        if (static_cast<int>(boundedBitrate) == encoder.bitrate()) {
            return true;
        }
        if (!encoder.setBitrate(static_cast<int>(boundedBitrate))) {
            if (error != nullptr) {
                *error = "video encoder bitrate update failed";
            }
            return false;
        }
        appliedBitrateBps.store(static_cast<uint32_t>(encoder.bitrate()), std::memory_order_release);
        bitrateReconfigureCount.fetch_add(1, std::memory_order_acq_rel);
        storeApplyDelay(targetBitrateUpdatedAtMs, lastBitrateApplyDelayMs, nowMs);
        return true;
    }

    if (!encoder.configure(targetWidth,
                           targetHeight,
                           targetFrameRate,
                           static_cast<int>(boundedBitrate),
                           payloadType,
                           preset)) {
        if (error != nullptr) {
            *error = "video encoder adaptive profile reconfigure failed";
        }
        return false;
    }

    encoder.requestKeyframe();
    appliedBitrateBps.store(static_cast<uint32_t>(encoder.bitrate()), std::memory_order_release);
    bitrateReconfigureCount.fetch_add(1, std::memory_order_acq_rel);
    storeApplyDelay(targetBitrateUpdatedAtMs, lastBitrateApplyDelayMs, nowMs);
    return true;
}

bool adaptVideoSendInputFrame(const VideoSendPipelineInputFrame& inputFrame,
                              int targetWidth,
                              int targetHeight,
                              VideoSendPipelineInputFrame& outFrame,
                              std::string* error) {
    targetWidth = normalizeEvenDimension(targetWidth);
    targetHeight = normalizeEvenDimension(targetHeight);
    outFrame = VideoSendPipelineInputFrame{};

    if (inputFrame.hasAvFrame()) {
        av::AVFramePtr scaled;
        if (!scaleAvFrame(*inputFrame.avFrame, targetWidth, targetHeight, scaled)) {
            if (error != nullptr) {
                *error = "video adaptive AVFrame scale failed";
            }
            return false;
        }
        outFrame.avFrame = std::move(scaled);
        return true;
    }

    if (inputFrame.hasScreenFrame()) {
        av::capture::ScreenFrame scaled;
        if (!scaleScreenFrame(inputFrame.screenFrame, targetWidth, targetHeight, scaled)) {
            if (error != nullptr) {
                *error = "video adaptive screen frame scale failed";
            }
            return false;
        }
        outFrame.screenFrame = std::move(scaled);
        return true;
    }

    if (error != nullptr) {
        *error = "empty video send input frame";
    }
    return false;
}

}  // namespace av::session
