#include "av/FFmpegUtils.h"
#include "av/codec/VideoEncoder.h"
#include "av/session/VideoSendPipeline.h"
#include "net/media/RTPSender.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFrameRate = 15;
constexpr int kBitrate = 900 * 1000;
constexpr std::size_t kMaxPayloadBytes = 1200;
constexpr uint8_t kPayloadType = 96;

bool fillSyntheticFrame(AVFrame& frame, int frameIndex) {
    const AVPixelFormat pixelFormat = static_cast<AVPixelFormat>(frame.format);
    if (frame.width <= 0 || frame.height <= 0 || frame.data[0] == nullptr) {
        return false;
    }

    const int width = frame.width;
    const int height = frame.height;
    const int minYStride = pixelFormat == AV_PIX_FMT_BGRA ? width * 4 : width;
    if (frame.linesize[0] < minYStride) {
        return false;
    }
    if (pixelFormat == AV_PIX_FMT_BGRA) {
        for (int y = 0; y < height; ++y) {
            uint8_t* dst = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
            for (int x = 0; x < width; ++x) {
                uint8_t* px = dst + static_cast<std::ptrdiff_t>(x) * 4;
                px[0] = static_cast<uint8_t>((x + frameIndex * 3) & 0xFF);
                px[1] = static_cast<uint8_t>((y + frameIndex * 5) & 0xFF);
                px[2] = static_cast<uint8_t>((x + y + frameIndex * 7) & 0xFF);
                px[3] = 0xFF;
            }
        }
        return true;
    }
    for (int y = 0; y < height; ++y) {
        uint8_t* dst = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        for (int x = 0; x < width; ++x) {
            dst[x] = static_cast<uint8_t>((x + y + frameIndex * 7) & 0xFF);
        }
    }

    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    if (pixelFormat == AV_PIX_FMT_NV12) {
        if (frame.data[1] == nullptr || frame.linesize[1] < width) {
            return false;
        }
        for (int y = 0; y < chromaHeight; ++y) {
            uint8_t* dst = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
            for (int x = 0; x < chromaWidth; ++x) {
                dst[2 * x] = static_cast<uint8_t>(96 + ((x + frameIndex) & 0x1F));
                dst[2 * x + 1] = static_cast<uint8_t>(160 + ((y + frameIndex) & 0x1F));
            }
        }
        return true;
    }

    if (pixelFormat == AV_PIX_FMT_YUV420P) {
        if (frame.data[1] == nullptr || frame.data[2] == nullptr ||
            frame.linesize[1] < chromaWidth || frame.linesize[2] < chromaWidth) {
            return false;
        }
        for (int y = 0; y < chromaHeight; ++y) {
            uint8_t* dstU = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
            uint8_t* dstV = frame.data[2] + static_cast<std::ptrdiff_t>(y) * frame.linesize[2];
            for (int x = 0; x < chromaWidth; ++x) {
                dstU[x] = static_cast<uint8_t>(96 + ((x + frameIndex) & 0x1F));
                dstV[x] = static_cast<uint8_t>(160 + ((y + frameIndex) & 0x1F));
            }
        }
        return true;
    }

    return false;
}

bool encodeFormat(av::codec::VideoEncoder& encoder,
                  av::session::VideoSendPipeline& pipeline,
                  media::RTPSender& sender,
                  AVPixelFormat pixelFormat,
                  std::string* error) {
    av::AVFramePtr frame = av::makeFrame();
    if (!frame) {
        if (error != nullptr) {
            *error = "frame alloc failed";
        }
        return false;
    }

    frame->format = static_cast<int>(pixelFormat);
    frame->width = kWidth;
    frame->height = kHeight;
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        if (error != nullptr) {
            *error = "frame buffer alloc failed";
        }
        return false;
    }

    for (int i = 0; i < 32; ++i) {
        if (av_frame_make_writable(frame.get()) < 0) {
            if (error != nullptr) {
                *error = "frame not writable";
            }
            return false;
        }
        frame->pts = i;
        if (!fillSyntheticFrame(*frame, i)) {
            if (error != nullptr) {
                *error = "fill frame failed";
            }
            return false;
        }

        std::vector<av::session::VideoSendPipelinePacket> packets;
        bool encodedKeyFrame = false;
        std::string pipelineError;
        if (pipeline.encodeAndPacketize(
                encoder,
                *frame,
                kPayloadType,
                i == 0,
                sender,
                packets,
                &encodedKeyFrame,
                &pipelineError)) {
            if (packets.empty()) {
                if (error != nullptr) {
                    *error = "encode succeeded but packet list is empty";
                }
                return false;
            }
            return true;
        }
        if (!pipelineError.empty()) {
            if (error != nullptr) {
                *error = pipelineError;
            }
            return false;
        }
    }

    if (error != nullptr) {
        *error = "no packet produced after multiple AVFrame submits";
    }
    return false;
}

}  // namespace

int main() {
    av::codec::VideoEncoder encoder;
    if (!encoder.configure(kWidth, kHeight, kFrameRate, kBitrate, kPayloadType)) {
        std::cerr << "SKIP video encoder configure failed" << std::endl;
        return 77;
    }

    av::session::VideoSendPipeline pipeline(
        av::session::VideoSendPipelineConfig{kFrameRate, kMaxPayloadBytes});
    media::RTPSender sender(0, 0x12345678);

    std::string error;
    if (!encodeFormat(encoder, pipeline, sender, AV_PIX_FMT_NV12, &error)) {
        std::cerr << "nv12 encode failed: " << error << std::endl;
        return 1;
    }
    if (!encodeFormat(encoder, pipeline, sender, AV_PIX_FMT_YUV420P, &error)) {
        std::cerr << "yuv420p encode failed: " << error << std::endl;
        return 1;
    }
    if (!encodeFormat(encoder, pipeline, sender, AV_PIX_FMT_BGRA, &error)) {
        std::cerr << "bgra encode failed: " << error << std::endl;
        return 1;
    }

    return 0;
}
