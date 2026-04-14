#include "VideoDecoder.h"

#include <algorithm>
#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace av::codec {
namespace {

std::string describeAvError(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return buffer;
}

bool copyNv12Frame(const AVFrame& frame, DecodedVideoFrame& outFrame) {
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }

    outFrame.width = frame.width;
    outFrame.height = frame.height;
    outFrame.pts = frame.best_effort_timestamp != AV_NOPTS_VALUE ? frame.best_effort_timestamp : frame.pts;
    outFrame.yPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    outFrame.uvPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height / 2));

    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* src = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        std::copy(src, src + frame.width, outFrame.yPlane.begin() + static_cast<std::ptrdiff_t>(y * frame.width));
    }

    for (int y = 0; y < frame.height / 2; ++y) {
        const uint8_t* src = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
        std::copy(src, src + frame.width, outFrame.uvPlane.begin() + static_cast<std::ptrdiff_t>(y * frame.width));
    }
    return true;
}

bool copyYuv420pFrameAsNv12(const AVFrame& frame, DecodedVideoFrame& outFrame) {
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }

    outFrame.width = frame.width;
    outFrame.height = frame.height;
    outFrame.pts = frame.best_effort_timestamp != AV_NOPTS_VALUE ? frame.best_effort_timestamp : frame.pts;
    outFrame.yPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    outFrame.uvPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height / 2));

    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* src = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        std::copy(src, src + frame.width, outFrame.yPlane.begin() + static_cast<std::ptrdiff_t>(y * frame.width));
    }

    for (int y = 0; y < frame.height / 2; ++y) {
        const uint8_t* srcU = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
        const uint8_t* srcV = frame.data[2] + static_cast<std::ptrdiff_t>(y) * frame.linesize[2];
        uint8_t* dst = outFrame.uvPlane.data() + static_cast<std::ptrdiff_t>(y * frame.width);
        for (int x = 0; x < frame.width / 2; ++x) {
            dst[x * 2] = srcU[x];
            dst[x * 2 + 1] = srcV[x];
        }
    }
    return true;
}

}  // namespace

void VideoDecoder::AvBufferRefDeleter::operator()(AVBufferRef* ref) const {
    if (ref != nullptr) {
        av_buffer_unref(&ref);
    }
}

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() = default;

AVPixelFormat VideoDecoder::selectPixelFormat(AVCodecContext* context, const AVPixelFormat* formats) {
    if (formats == nullptr) {
        return AV_PIX_FMT_NONE;
    }

    const auto* decoder = static_cast<const VideoDecoder*>(context != nullptr ? context->opaque : nullptr);
    if (decoder != nullptr && decoder->m_hwPixelFormat != AV_PIX_FMT_NONE) {
        for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == decoder->m_hwPixelFormat) {
                return *format;
            }
        }
    }

    return formats[0];
}

bool VideoDecoder::configureHardwareDecode(const AVCodec* codec, AVCodecContext& context) {
    if (codec == nullptr) {
        return false;
    }

    for (int index = 0;; ++index) {
        const AVCodecHWConfig* hwConfig = avcodec_get_hw_config(codec, index);
        if (hwConfig == nullptr) {
            break;
        }

        if ((hwConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0) {
            continue;
        }

        AVBufferRef* rawDeviceContext = nullptr;
        const int createResult = av_hwdevice_ctx_create(&rawDeviceContext, hwConfig->device_type, nullptr, nullptr, 0);
        if (createResult < 0 || rawDeviceContext == nullptr) {
            continue;
        }

        m_hwDeviceContext.reset(rawDeviceContext);
        context.hw_device_ctx = av_buffer_ref(m_hwDeviceContext.get());
        if (context.hw_device_ctx == nullptr) {
            m_hwDeviceContext.reset();
            continue;
        }

        m_hwPixelFormat = hwConfig->pix_fmt;
        context.get_format = &VideoDecoder::selectPixelFormat;
        context.opaque = this;
        return true;
    }

    return false;
}

bool VideoDecoder::configure() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        return false;
    }

    av::AVCodecContextPtr context = av::makeCodecContext(codec);
    if (!context) {
        return false;
    }

    m_hwDeviceContext.reset();
    m_hwPixelFormat = AV_PIX_FMT_NONE;

    context->thread_count = 1;
    context->thread_type = FF_THREAD_FRAME;
    context->opaque = nullptr;

    (void)configureHardwareDecode(codec, *context);

    if (avcodec_open2(context.get(), codec, nullptr) < 0) {
        m_hwDeviceContext.reset();
        m_hwPixelFormat = AV_PIX_FMT_NONE;
        return false;
    }

    m_codecContext = std::move(context);
    return true;
}

bool VideoDecoder::decode(const EncodedVideoFrame& inFrame, DecodedVideoFrame& outFrame, std::string* error) {
    if (!m_codecContext && !configure()) {
        if (error != nullptr) {
            *error = "decoder configure failed";
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

    packet->data = const_cast<uint8_t*>(inFrame.payload.data());
    packet->size = static_cast<int>(inFrame.payload.size());
    packet->pts = inFrame.pts;
    packet->dts = inFrame.pts;

    const int sendResult = avcodec_send_packet(m_codecContext.get(), packet.get());
    if (sendResult < 0) {
        if (error != nullptr) {
            *error = "avcodec_send_packet failed: " + describeAvError(sendResult);
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

    const int receiveResult = avcodec_receive_frame(m_codecContext.get(), frame.get());
    if (receiveResult < 0) {
        if (error != nullptr) {
            *error = "avcodec_receive_frame failed: " + describeAvError(receiveResult);
        }
        return false;
    }

    const AVFrame* resolvedFrame = frame.get();
    av::AVFramePtr transferredFrame;
    if (m_hwPixelFormat != AV_PIX_FMT_NONE && frame->format == m_hwPixelFormat) {
        transferredFrame = av::makeFrame();
        if (!transferredFrame) {
            if (error != nullptr) {
                *error = "hw transfer frame alloc failed";
            }
            return false;
        }

        const int transferResult = av_hwframe_transfer_data(transferredFrame.get(), frame.get(), 0);
        if (transferResult < 0) {
            if (error != nullptr) {
                *error = "av_hwframe_transfer_data failed: " + describeAvError(transferResult);
            }
            return false;
        }

        (void)av_frame_copy_props(transferredFrame.get(), frame.get());
        resolvedFrame = transferredFrame.get();
    }

    if (resolvedFrame->format == AV_PIX_FMT_NV12) {
        return copyNv12Frame(*resolvedFrame, outFrame);
    }
    if (resolvedFrame->format == AV_PIX_FMT_YUV420P || resolvedFrame->format == AV_PIX_FMT_YUVJ420P) {
        return copyYuv420pFrameAsNv12(*resolvedFrame, outFrame);
    }

    if (error != nullptr) {
        *error = "unsupported decoded pixel format";
    }
    return false;
}

}  // namespace av::codec

