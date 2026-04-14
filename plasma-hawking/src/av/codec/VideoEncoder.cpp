#include "VideoEncoder.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/opt.h>
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

std::vector<const char*> orderedCodecCandidates() {
    std::vector<const char*> ordered;
    const auto pushUnique = [&ordered](const char* candidate) {
        if (candidate == nullptr || *candidate == '\0') {
            return;
        }
        if (std::find_if(ordered.begin(), ordered.end(),
                         [candidate](const char* existing) { return std::string(existing) == candidate; }) != ordered.end()) {
            return;
        }
        ordered.push_back(candidate);
    };

    pushUnique(std::getenv("MEETING_VIDEO_ENCODER"));
    for (const char* candidate : kCodecCandidates) {
        pushUnique(candidate);
    }
    return ordered;
}

uint8_t clampToByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint8_t lumaFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    const int y = ((66 * static_cast<int>(r)) + (129 * static_cast<int>(g)) + (25 * static_cast<int>(b)) + 128) >> 8;
    return clampToByte(y + 16);
}

uint8_t chromaUFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    const int u = ((-38 * static_cast<int>(r)) - (74 * static_cast<int>(g)) + (112 * static_cast<int>(b)) + 128) >> 8;
    return clampToByte(u + 128);
}

uint8_t chromaVFromBgra(uint8_t b, uint8_t g, uint8_t r) {
    const int v = ((112 * static_cast<int>(r)) - (94 * static_cast<int>(g)) - (18 * static_cast<int>(b)) + 128) >> 8;
    return clampToByte(v + 128);
}

bool fillFrameFromBgra(const capture::ScreenFrame& inFrame, AVFrame& outFrame, AVPixelFormat pixelFormat) {
    if (inFrame.width <= 0 || inFrame.height <= 0 || (inFrame.width % 2) != 0 || (inFrame.height % 2) != 0) {
        return false;
    }
    if (pixelFormat != AV_PIX_FMT_NV12 && pixelFormat != AV_PIX_FMT_YUV420P) {
        return false;
    }

    const uint8_t* source = inFrame.bgra.data();
    const int width = inFrame.width;
    const int height = inFrame.height;
    const int sourceStride = width * 4;

    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = source + static_cast<std::ptrdiff_t>(y) * sourceStride;
        uint8_t* yRow = outFrame.data[0] + static_cast<std::ptrdiff_t>(y) * outFrame.linesize[0];
        for (int x = 0; x < width; ++x) {
            const uint8_t* pixel = srcRow + static_cast<std::ptrdiff_t>(x) * 4;
            yRow[x] = lumaFromBgra(pixel[0], pixel[1], pixel[2]);
        }
    }

    for (int y = 0; y < height; y += 2) {
        const uint8_t* srcRow0 = source + static_cast<std::ptrdiff_t>(y) * sourceStride;
        const uint8_t* srcRow1 = source + static_cast<std::ptrdiff_t>(std::min(y + 1, height - 1)) * sourceStride;

        uint8_t* uvRowInterleaved = nullptr;
        uint8_t* uRowPlanar = nullptr;
        uint8_t* vRowPlanar = nullptr;
        if (pixelFormat == AV_PIX_FMT_NV12) {
            uvRowInterleaved = outFrame.data[1] + static_cast<std::ptrdiff_t>(y / 2) * outFrame.linesize[1];
        } else {
            uRowPlanar = outFrame.data[1] + static_cast<std::ptrdiff_t>(y / 2) * outFrame.linesize[1];
            vRowPlanar = outFrame.data[2] + static_cast<std::ptrdiff_t>(y / 2) * outFrame.linesize[2];
        }

        for (int x = 0; x < width; x += 2) {
            const uint8_t* p00 = srcRow0 + static_cast<std::ptrdiff_t>(x) * 4;
            const uint8_t* p01 = srcRow0 + static_cast<std::ptrdiff_t>(std::min(x + 1, width - 1)) * 4;
            const uint8_t* p10 = srcRow1 + static_cast<std::ptrdiff_t>(x) * 4;
            const uint8_t* p11 = srcRow1 + static_cast<std::ptrdiff_t>(std::min(x + 1, width - 1)) * 4;

            const int uAvg = (static_cast<int>(chromaUFromBgra(p00[0], p00[1], p00[2])) +
                              static_cast<int>(chromaUFromBgra(p01[0], p01[1], p01[2])) +
                              static_cast<int>(chromaUFromBgra(p10[0], p10[1], p10[2])) +
                              static_cast<int>(chromaUFromBgra(p11[0], p11[1], p11[2])) +
                              2) /
                             4;
            const int vAvg = (static_cast<int>(chromaVFromBgra(p00[0], p00[1], p00[2])) +
                              static_cast<int>(chromaVFromBgra(p01[0], p01[1], p01[2])) +
                              static_cast<int>(chromaVFromBgra(p10[0], p10[1], p10[2])) +
                              static_cast<int>(chromaVFromBgra(p11[0], p11[1], p11[2])) +
                              2) /
                             4;

            if (pixelFormat == AV_PIX_FMT_NV12) {
                uvRowInterleaved[x] = clampToByte(uAvg);
                uvRowInterleaved[x + 1] = clampToByte(vAvg);
            } else {
                uRowPlanar[x / 2] = clampToByte(uAvg);
                vRowPlanar[x / 2] = clampToByte(vAvg);
            }
        }
    }

    return true;
}

}  // namespace

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

    for (const char* candidate : orderedCodecCandidates()) {
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

            m_width = width;
            m_height = height;
            m_frameRate = frameRate;
            m_bitrate = bitrate;
            m_payloadType = kScreenSharePayloadType;
            m_outputPixelFormat = outputFormat;
            m_codecContext = std::move(context);
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

    if (!fillFrameFromBgra(inFrame, *frame, m_outputPixelFormat)) {
        if (error != nullptr) {
            *error = "BGRA to encoder pixel format conversion failed";
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
    if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
        if (error != nullptr) {
            error->clear();
        }
        return false;
    }
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

