#include "VideoEncoder.h"

#include <algorithm>
#include <array>
#include <string>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace av::codec {
namespace {

constexpr uint8_t kScreenSharePayloadType = 97;
constexpr std::array<const char*, 5> kCodecCandidates = {
    "h264_nvenc",
    "h264_amf",
    "h264_qsv",
    "libx264",
    "h264",
};
constexpr std::array<AVPixelFormat, 2> kPixelCandidates = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NV12,
};

std::string describeAvError(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return buffer;
}

bool codecSupportsPixelFormat(const AVCodec* codec, AVPixelFormat pixelFormat) {
    if (codec == nullptr || codec->pix_fmts == nullptr) {
        return true;
    }
    for (const AVPixelFormat* format = codec->pix_fmts; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == pixelFormat) {
            return true;
        }
    }
    return false;
}

}  // namespace

void VideoEncoder::SwsContextDeleter::operator()(SwsContext* ctx) const {
    if (ctx != nullptr) {
        sws_freeContext(ctx);
    }
}

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::configure(int width, int height, int frameRate, int bitrate) {
    if (width <= 0 || height <= 0 || frameRate <= 0 || bitrate <= 0) {
        return false;
    }

    width &= ~1;
    height &= ~1;
    width = std::max(2, width);
    height = std::max(2, height);

    for (const char* candidate : kCodecCandidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(candidate);
        if (codec == nullptr) {
            continue;
        }

        for (AVPixelFormat outputFormat : kPixelCandidates) {
            if (!codecSupportsPixelFormat(codec, outputFormat)) {
                continue;
            }

            av::AVCodecContextPtr context = av::makeCodecContext(codec);
            if (!context) {
                continue;
            }

            context->codec_id = codec->id;
            context->codec_type = AVMEDIA_TYPE_VIDEO;
            context->bit_rate = bitrate;
            context->width = width;
            context->height = height;
            context->time_base = AVRational{1, frameRate};
            context->framerate = AVRational{frameRate, 1};
            context->gop_size = std::max(frameRate * 2, frameRate);
            context->max_b_frames = 0;
            context->pix_fmt = outputFormat;

            av_opt_set(context->priv_data, "preset", "ultrafast", 0);
            av_opt_set(context->priv_data, "tune", "zerolatency", 0);
            av_opt_set(context->priv_data, "repeat_headers", "1", 0);

            if (avcodec_open2(context.get(), codec, nullptr) < 0) {
                continue;
            }

            SwsContext* sws = sws_getCachedContext(nullptr,
                                                   width,
                                                   height,
                                                   AV_PIX_FMT_BGRA,
                                                   width,
                                                   height,
                                                   outputFormat,
                                                   SWS_FAST_BILINEAR,
                                                   nullptr,
                                                   nullptr,
                                                   nullptr);
            if (sws == nullptr) {
                continue;
            }

            m_width = width;
            m_height = height;
            m_frameRate = frameRate;
            m_bitrate = bitrate;
            m_payloadType = kScreenSharePayloadType;
            m_outputPixelFormat = outputFormat;
            m_codecContext = std::move(context);
            m_swsContext.reset(sws);
            m_codecName = candidate;
            return true;
        }
    }

    return false;
}

bool VideoEncoder::encode(const capture::ScreenFrame& inFrame,
                          EncodedVideoFrame& outFrame,
                          bool forceKeyFrame,
                          std::string* error) {
    if (!m_codecContext) {
        if (!configure(inFrame.width, inFrame.height, 5, 1500 * 1000)) {
            if (error != nullptr) {
                *error = "encoder configure failed";
            }
            return false;
        }
    }

    if (inFrame.width != m_width || inFrame.height != m_height) {
        if (error != nullptr) {
            *error = "input size mismatch";
        }
        return false;
    }

    const std::size_t expectedBytes = static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 4U;
    if (inFrame.bgra.size() != expectedBytes) {
        if (error != nullptr) {
            *error = "input frame bytes mismatch";
        }
        return false;
    }

    av::AVFramePtr frame = av::makeFrame();
    if (!frame) {
        if (error != nullptr) {
            *error = "frame alloc failed";
        }
        return false;
    }

    frame->format = static_cast<int>(m_outputPixelFormat);
    frame->width = m_width;
    frame->height = m_height;
    frame->pts = inFrame.pts;
    if (forceKeyFrame) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->key_frame = 1;
    }
    if (av_frame_get_buffer(frame.get(), 32) < 0) {
        if (error != nullptr) {
            *error = "frame buffer alloc failed";
        }
        return false;
    }

    const uint8_t* srcSlices[4] = {inFrame.bgra.data(), nullptr, nullptr, nullptr};
    const int srcStride[4] = {m_width * 4, 0, 0, 0};
    if (sws_scale(m_swsContext.get(),
                  srcSlices,
                  srcStride,
                  0,
                  m_height,
                  frame->data,
                  frame->linesize) <= 0) {
        if (error != nullptr) {
            *error = "sws_scale failed";
        }
        return false;
    }

    const int sendResult = avcodec_send_frame(m_codecContext.get(), frame.get());
    if (sendResult < 0) {
        if (error != nullptr) {
            *error = "avcodec_send_frame failed: " + describeAvError(sendResult);
        }
        return false;
    }

    av::AVPacketPtr packet = av::makePacket();
    if (!packet) {
        if (error != nullptr) {
            *error = "packet alloc failed";
        }
        return false;
    }

    const int receiveResult = avcodec_receive_packet(m_codecContext.get(), packet.get());
    if (receiveResult < 0) {
        if (error != nullptr) {
            *error = "avcodec_receive_packet failed: " + describeAvError(receiveResult);
        }
        return false;
    }

    outFrame.width = m_width;
    outFrame.height = m_height;
    outFrame.frameRate = m_frameRate;
    outFrame.pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : inFrame.pts;
    outFrame.keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    outFrame.payloadType = m_payloadType;
    outFrame.payload.assign(packet->data, packet->data + packet->size);
    if (outFrame.payload.empty()) {
        if (error != nullptr) {
            *error = "empty video packet";
        }
        return false;
    }

    return true;
}

int VideoEncoder::width() const {
    return m_width;
}

int VideoEncoder::height() const {
    return m_height;
}

int VideoEncoder::frameRate() const {
    return m_frameRate;
}

int VideoEncoder::bitrate() const {
    return m_bitrate;
}

uint8_t VideoEncoder::payloadType() const {
    return m_payloadType;
}

}  // namespace av::codec
