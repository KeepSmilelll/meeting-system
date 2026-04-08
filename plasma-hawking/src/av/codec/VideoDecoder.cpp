#include "VideoDecoder.h"

#include <algorithm>
#include <string>

extern "C" {
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

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::configure() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        return false;
    }

    av::AVCodecContextPtr context = av::makeCodecContext(codec);
    if (!context) {
        return false;
    }

    context->thread_count = 1;
    context->thread_type = FF_THREAD_FRAME;
    if (avcodec_open2(context.get(), codec, nullptr) < 0) {
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

    if (frame->format == AV_PIX_FMT_NV12) {
        return copyNv12Frame(*frame, outFrame);
    }
    if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
        return copyYuv420pFrameAsNv12(*frame, outFrame);
    }

    if (error != nullptr) {
        *error = "unsupported decoded pixel format";
    }
    return false;
}

}  // namespace av::codec
