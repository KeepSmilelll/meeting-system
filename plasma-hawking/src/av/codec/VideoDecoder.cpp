#include "VideoDecoder.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
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

bool parseEnvBool(const char* rawValue, bool defaultValue) {
    if (rawValue == nullptr) {
        return defaultValue;
    }
    std::string normalized(rawValue);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (normalized.empty()) {
        return defaultValue;
    }
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return std::atoi(rawValue) != 0;
}

bool envFlag(const char* key, bool defaultValue) {
    return parseEnvBool(std::getenv(key), defaultValue);
}

bool canRetainFrameReference(const AVFrame& frame) {
    for (int index = 0; index < AV_NUM_DATA_POINTERS; ++index) {
        if (frame.buf[index] != nullptr) {
            return true;
        }
    }
    for (int index = 0; index < frame.nb_extended_buf; ++index) {
        if (frame.extended_buf != nullptr && frame.extended_buf[index] != nullptr) {
            return true;
        }
    }
    return false;
}

av::codec::DecodedVideoFrame::SharedAvFrame adoptOwnedFrame(av::AVFramePtr&& frame) {
    AVFrame* rawFrame = frame.release();
    if (rawFrame == nullptr) {
        return {};
    }
    return av::codec::DecodedVideoFrame::SharedAvFrame(
        rawFrame,
        [](AVFrame* ownedFrame) {
            av_frame_free(&ownedFrame);
        });
}

void assignFrameMetadata(const AVFrame& frame, DecodedVideoFrame& outFrame) {
    outFrame.width = frame.width;
    outFrame.height = frame.height;
    outFrame.pts = frame.best_effort_timestamp != AV_NOPTS_VALUE ? frame.best_effort_timestamp : frame.pts;
    outFrame.avFrame.reset();
    outFrame.pixelFormat = static_cast<AVPixelFormat>(frame.format);
}

DecodedVideoFrame::HardwareFrameKind detectHardwareFrameKind(const AVFrame& frame) {
#ifdef _WIN32
    if (static_cast<AVPixelFormat>(frame.format) == AV_PIX_FMT_D3D11 && frame.data[0] != nullptr) {
        return DecodedVideoFrame::HardwareFrameKind::D3d11Texture2D;
    }
#else
    (void)frame;
#endif
    return DecodedVideoFrame::HardwareFrameKind::None;
}

bool captureHardwareFrameShareCandidate(const AVFrame& frame, DecodedVideoFrame& outFrame) {
    const DecodedVideoFrame::HardwareFrameKind frameKind = detectHardwareFrameKind(frame);
    if (frameKind == DecodedVideoFrame::HardwareFrameKind::None) {
        return false;
    }

    av::AVFramePtr retainedFrame = av::makeFrame();
    if (!retainedFrame) {
        return false;
    }
    if (av_frame_ref(retainedFrame.get(), &frame) < 0) {
        return false;
    }

    outFrame.hardwareAvFrame = adoptOwnedFrame(std::move(retainedFrame));
    if (!outFrame.hardwareAvFrame) {
        return false;
    }

    outFrame.hardwareFrameKind = frameKind;
    outFrame.hardwareTextureHandle = frame.data[0];
    outFrame.hardwareSubresourceIndex = static_cast<uint32_t>(
        reinterpret_cast<std::uintptr_t>(frame.data[1]));
    return true;
}

bool moveNv12Frame(av::AVFramePtr&& frame, DecodedVideoFrame& outFrame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }
    if (!canRetainFrameReference(*frame)) {
        return false;
    }

    assignFrameMetadata(*frame, outFrame);
    outFrame.yPlane.clear();
    outFrame.uvPlane.clear();
    outFrame.uPlane.clear();
    outFrame.vPlane.clear();
    outFrame.pixelFormat = AV_PIX_FMT_NV12;
    outFrame.avFrame = adoptOwnedFrame(std::move(frame));
    return static_cast<bool>(outFrame.avFrame);
}

bool moveYuv420pFrame(av::AVFramePtr&& frame, DecodedVideoFrame& outFrame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }
    if (!canRetainFrameReference(*frame)) {
        return false;
    }

    assignFrameMetadata(*frame, outFrame);
    outFrame.yPlane.clear();
    outFrame.uvPlane.clear();
    outFrame.uPlane.clear();
    outFrame.vPlane.clear();
    outFrame.pixelFormat = static_cast<AVPixelFormat>(frame->format);
    outFrame.avFrame = adoptOwnedFrame(std::move(frame));
    return static_cast<bool>(outFrame.avFrame);
}

bool copyNv12Frame(const AVFrame& frame, DecodedVideoFrame& outFrame) {
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }

    assignFrameMetadata(frame, outFrame);
    outFrame.avFrame.reset();
    outFrame.pixelFormat = AV_PIX_FMT_NV12;
    outFrame.yPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    outFrame.uvPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height / 2));
    outFrame.uPlane.clear();
    outFrame.vPlane.clear();

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

bool copyYuv420pFrame(const AVFrame& frame, DecodedVideoFrame& outFrame) {
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }

    assignFrameMetadata(frame, outFrame);
    outFrame.avFrame.reset();
    outFrame.pixelFormat = static_cast<AVPixelFormat>(frame.format);
    outFrame.yPlane.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    outFrame.uvPlane.clear();
    outFrame.uPlane.resize(static_cast<std::size_t>(frame.width / 2) * static_cast<std::size_t>(frame.height / 2));
    outFrame.vPlane.resize(static_cast<std::size_t>(frame.width / 2) * static_cast<std::size_t>(frame.height / 2));

    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* src = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        std::copy(src, src + frame.width, outFrame.yPlane.begin() + static_cast<std::ptrdiff_t>(y * frame.width));
    }

    for (int y = 0; y < frame.height / 2; ++y) {
        const uint8_t* srcU = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
        const uint8_t* srcV = frame.data[2] + static_cast<std::ptrdiff_t>(y) * frame.linesize[2];
        std::copy(srcU,
                  srcU + (frame.width / 2),
                  outFrame.uPlane.begin() + static_cast<std::ptrdiff_t>(y * (frame.width / 2)));
        std::copy(srcV,
                  srcV + (frame.width / 2),
                  outFrame.vPlane.begin() + static_cast<std::ptrdiff_t>(y * (frame.width / 2)));
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

    // R1 strategy:
    // - default prefer software decode to avoid GPU->CPU hwframe download without interop path.
    // - keep hardware decode capability via explicit MEETING_FORCE_HW_VIDEO_DECODE=1.
    m_enableHardwareTextureShare = envFlag("MEETING_ENABLE_HW_TEXTURE_SHARE", false);
    const bool disableHardwareDecode = envFlag("MEETING_DISABLE_HW_VIDEO_DECODE", false);
    const bool forceHardwareDecode = envFlag("MEETING_FORCE_HW_VIDEO_DECODE", false) ||
                                     m_enableHardwareTextureShare;
    const bool preferSoftwareDecode = envFlag("MEETING_PREFER_SOFTWARE_VIDEO_DECODE", true);
    const bool allowHardwareDecode = !m_forceSoftwareDecode &&
                                     !disableHardwareDecode &&
                                     (forceHardwareDecode || !preferSoftwareDecode);
    if (allowHardwareDecode) {
        (void)configureHardwareDecode(codec, *context);
    }

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

    outFrame = DecodedVideoFrame{};

    const auto fallbackToSoftwareDecode = [this]() {
        if (m_hwPixelFormat == AV_PIX_FMT_NONE && m_forceSoftwareDecode) {
            return false;
        }
        m_forceSoftwareDecode = true;
        m_codecContext.reset();
        m_hwDeviceContext.reset();
        m_hwPixelFormat = AV_PIX_FMT_NONE;
        return configure();
    };

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
        if (!m_forceSoftwareDecode && fallbackToSoftwareDecode()) {
            return decode(inFrame, outFrame, error);
        }
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
        if (!m_forceSoftwareDecode && fallbackToSoftwareDecode()) {
            return decode(inFrame, outFrame, error);
        }
        if (error != nullptr) {
            *error = "avcodec_receive_frame failed: " + describeAvError(receiveResult);
        }
        return false;
    }

    av::AVFramePtr transferredFrame;
    if (m_hwPixelFormat != AV_PIX_FMT_NONE && frame->format == m_hwPixelFormat) {
        const bool capturedShareCandidate = captureHardwareFrameShareCandidate(*frame, outFrame);
        if (capturedShareCandidate && m_enableHardwareTextureShare) {
            assignFrameMetadata(*frame, outFrame);
            return true;
        }

        transferredFrame = av::makeFrame();
        if (!transferredFrame) {
            if (error != nullptr) {
                *error = "hw transfer frame alloc failed";
            }
            return false;
        }

        const int transferResult = av_hwframe_transfer_data(transferredFrame.get(), frame.get(), 0);
        if (transferResult < 0) {
            if (!m_forceSoftwareDecode && fallbackToSoftwareDecode()) {
                return decode(inFrame, outFrame, error);
            }
            if (error != nullptr) {
                *error = "av_hwframe_transfer_data failed: " + describeAvError(transferResult);
            }
            return false;
        }

        (void)av_frame_copy_props(transferredFrame.get(), frame.get());
    }

    if (transferredFrame) {
        const AVPixelFormat transferredFormat = static_cast<AVPixelFormat>(transferredFrame->format);
        if (transferredFormat == AV_PIX_FMT_NV12) {
            if (moveNv12Frame(std::move(transferredFrame), outFrame)) {
                return true;
            }
            return copyNv12Frame(*transferredFrame, outFrame);
        }
        if (transferredFormat == AV_PIX_FMT_YUV420P || transferredFormat == AV_PIX_FMT_YUVJ420P) {
            if (moveYuv420pFrame(std::move(transferredFrame), outFrame)) {
                return true;
            }
            return copyYuv420pFrame(*transferredFrame, outFrame);
        }
    } else {
        const AVPixelFormat softwareFormat = static_cast<AVPixelFormat>(frame->format);
        if (softwareFormat == AV_PIX_FMT_NV12) {
            if (moveNv12Frame(std::move(frame), outFrame)) {
                return true;
            }
            return copyNv12Frame(*frame, outFrame);
        }
        if (softwareFormat == AV_PIX_FMT_YUV420P || softwareFormat == AV_PIX_FMT_YUVJ420P) {
            if (moveYuv420pFrame(std::move(frame), outFrame)) {
                return true;
            }
            return copyYuv420pFrame(*frame, outFrame);
        }
    }

    if (error != nullptr) {
        *error = "unsupported decoded pixel format";
    }
    return false;
}

}  // namespace av::codec

