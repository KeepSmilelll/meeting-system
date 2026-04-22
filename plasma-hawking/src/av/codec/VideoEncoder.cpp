#include "VideoEncoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
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

bool hasAnnexBStartCode(const std::vector<uint8_t>& payload) {
    for (std::size_t i = 0; i + 3 < payload.size(); ++i) {
        if (payload[i] == 0x00 && payload[i + 1] == 0x00 &&
            (payload[i + 2] == 0x01 || (i + 3 < payload.size() && payload[i + 2] == 0x00 && payload[i + 3] == 0x01))) {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> buildAnnexBParameterSetsFromExtradata(const uint8_t* extradata, int extradataSize) {
    std::vector<uint8_t> parameterSets;
    if (extradata == nullptr || extradataSize < 7 || extradata[0] != 1) {
        return parameterSets;
    }

    std::size_t offset = 5;
    const uint8_t spsCount = static_cast<uint8_t>(extradata[offset] & 0x1FU);
    ++offset;

    auto appendNalu = [&parameterSets](const uint8_t* data, std::size_t size) {
        if (data == nullptr || size == 0) {
            return;
        }
        static constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
        parameterSets.insert(parameterSets.end(), std::begin(kStartCode), std::end(kStartCode));
        parameterSets.insert(parameterSets.end(), data, data + static_cast<std::ptrdiff_t>(size));
    };

    auto readLength = [extradata, extradataSize](std::size_t& cursor, std::size_t& length) -> bool {
        if (cursor + 2 > static_cast<std::size_t>(extradataSize)) {
            return false;
        }
        length = static_cast<std::size_t>((extradata[cursor] << 8) | extradata[cursor + 1]);
        cursor += 2;
        if (cursor + length > static_cast<std::size_t>(extradataSize)) {
            return false;
        }
        return true;
    };

    for (uint8_t i = 0; i < spsCount; ++i) {
        std::size_t naluSize = 0;
        if (!readLength(offset, naluSize)) {
            return {};
        }
        appendNalu(extradata + offset, naluSize);
        offset += naluSize;
    }

    if (offset >= static_cast<std::size_t>(extradataSize)) {
        return parameterSets;
    }

    const uint8_t ppsCount = extradata[offset++];
    for (uint8_t i = 0; i < ppsCount; ++i) {
        std::size_t naluSize = 0;
        if (!readLength(offset, naluSize)) {
            return {};
        }
        appendNalu(extradata + offset, naluSize);
        offset += naluSize;
    }

    return parameterSets;
}

bool convertAvccPayloadToAnnexB(const std::vector<uint8_t>& payload, int nalLengthSize, std::vector<uint8_t>& annexBPayload) {
    if (payload.empty()) {
        annexBPayload.clear();
        return true;
    }

    if (hasAnnexBStartCode(payload)) {
        annexBPayload = payload;
        return true;
    }

    if (nalLengthSize < 1 || nalLengthSize > 4) {
        nalLengthSize = 4;
    }

    std::vector<uint8_t> converted;
    converted.reserve(payload.size() + 64);
    std::size_t cursor = 0;
    static constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
    while (cursor + static_cast<std::size_t>(nalLengthSize) <= payload.size()) {
        uint32_t naluSize = 0;
        for (int i = 0; i < nalLengthSize; ++i) {
            naluSize = (naluSize << 8) | payload[cursor + static_cast<std::size_t>(i)];
        }
        cursor += static_cast<std::size_t>(nalLengthSize);
        if (naluSize == 0 || cursor + naluSize > payload.size()) {
            annexBPayload = payload;
            return false;
        }

        converted.insert(converted.end(), std::begin(kStartCode), std::end(kStartCode));
        converted.insert(converted.end(),
                         payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                         payload.begin() + static_cast<std::ptrdiff_t>(cursor + naluSize));
        cursor += naluSize;
    }

    if (converted.empty() || cursor != payload.size()) {
        annexBPayload = payload;
        return false;
    }

    annexBPayload = std::move(converted);
    return true;
}

void prependParameterSetsForKeyFrame(std::vector<uint8_t>& payload,
                                     bool keyFrame,
                                     const uint8_t* extradata,
                                     int extradataSize) {
    if (!keyFrame) {
        return;
    }

    const auto parameterSets = buildAnnexBParameterSetsFromExtradata(extradata, extradataSize);
    if (parameterSets.empty()) {
        return;
    }

    payload.insert(payload.begin(), parameterSets.begin(), parameterSets.end());
}

void applyLowLatencyCodecOptions(const char* codecName, AVCodecContext* context, VideoEncoderPreset preset) {
    if (codecName == nullptr || context == nullptr) {
        return;
    }

    const std::string name(codecName);
    if (name.find("nvenc") != std::string::npos) {
        const char* presetValue = "p1";
        const char* tuneValue = "ull";
        const char* rcValue = "cbr";
        const char* zeroLatency = "1";
        switch (preset) {
        case VideoEncoderPreset::Balanced:
            presetValue = "p4";
            tuneValue = "ll";
            break;
        case VideoEncoderPreset::Quality:
            presetValue = "p7";
            tuneValue = "hq";
            rcValue = "vbr";
            zeroLatency = "0";
            break;
        case VideoEncoderPreset::Realtime:
        default:
            break;
        }
        av_opt_set(context->priv_data, "preset", presetValue, 0);
        av_opt_set(context->priv_data, "tune", tuneValue, 0);
        av_opt_set(context->priv_data, "zerolatency", zeroLatency, 0);
        av_opt_set(context->priv_data, "rc", rcValue, 0);
        return;
    }

    if (name.find("libx264") != std::string::npos || name == "h264") {
        const char* presetValue = "ultrafast";
        switch (preset) {
        case VideoEncoderPreset::Balanced:
            presetValue = "veryfast";
            break;
        case VideoEncoderPreset::Quality:
            presetValue = "medium";
            break;
        case VideoEncoderPreset::Realtime:
        default:
            break;
        }
        av_opt_set(context->priv_data, "preset", presetValue, 0);
        av_opt_set(context->priv_data, "tune", "zerolatency", 0);
        if (preset == VideoEncoderPreset::Quality) {
            av_opt_set(context->priv_data, "profile", "high", 0);
        }
    }
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

bool fillFrameFromBgraInput(const AVFrame& inFrame, AVFrame& outFrame, AVPixelFormat pixelFormat) {
    if (inFrame.width <= 0 || inFrame.height <= 0 || (inFrame.width % 2) != 0 || (inFrame.height % 2) != 0) {
        return false;
    }
    if (pixelFormat != AV_PIX_FMT_NV12 && pixelFormat != AV_PIX_FMT_YUV420P) {
        return false;
    }
    if (inFrame.data[0] == nullptr || inFrame.linesize[0] < inFrame.width * 4) {
        return false;
    }

    const uint8_t* source = inFrame.data[0];
    const int width = inFrame.width;
    const int height = inFrame.height;
    const int sourceStride = inFrame.linesize[0];

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
                              2) / 4;
            const int vAvg = (static_cast<int>(chromaVFromBgra(p00[0], p00[1], p00[2])) +
                              static_cast<int>(chromaVFromBgra(p01[0], p01[1], p01[2])) +
                              static_cast<int>(chromaVFromBgra(p10[0], p10[1], p10[2])) +
                              static_cast<int>(chromaVFromBgra(p11[0], p11[1], p11[2])) +
                              2) / 4;

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

bool fillFrameFromYuvInput(const AVFrame& inFrame, AVFrame& outFrame, AVPixelFormat outputPixelFormat) {
    const AVPixelFormat inputPixelFormat = static_cast<AVPixelFormat>(inFrame.format);
    if ((inputPixelFormat != AV_PIX_FMT_NV12 && inputPixelFormat != AV_PIX_FMT_YUV420P) ||
        (outputPixelFormat != AV_PIX_FMT_NV12 && outputPixelFormat != AV_PIX_FMT_YUV420P)) {
        return false;
    }

    const int width = inFrame.width;
    const int height = inFrame.height;
    if (width <= 0 || height <= 0 || (width % 2) != 0 || (height % 2) != 0) {
        return false;
    }

    if (!copyPlaneRows(outFrame.data[0], outFrame.linesize[0], inFrame.data[0], inFrame.linesize[0], width, height)) {
        return false;
    }

    const int chromaHeight = height / 2;
    const int chromaWidth = width / 2;
    if (inputPixelFormat == AV_PIX_FMT_NV12 && outputPixelFormat == AV_PIX_FMT_NV12) {
        return copyPlaneRows(outFrame.data[1], outFrame.linesize[1], inFrame.data[1], inFrame.linesize[1], width, chromaHeight);
    }
    if (inputPixelFormat == AV_PIX_FMT_YUV420P && outputPixelFormat == AV_PIX_FMT_YUV420P) {
        return copyPlaneRows(outFrame.data[1], outFrame.linesize[1], inFrame.data[1], inFrame.linesize[1], chromaWidth, chromaHeight) &&
               copyPlaneRows(outFrame.data[2], outFrame.linesize[2], inFrame.data[2], inFrame.linesize[2], chromaWidth, chromaHeight);
    }
    if (inputPixelFormat == AV_PIX_FMT_NV12 && outputPixelFormat == AV_PIX_FMT_YUV420P) {
        if (inFrame.data[1] == nullptr || outFrame.data[1] == nullptr || outFrame.data[2] == nullptr) {
            return false;
        }
        for (int y = 0; y < chromaHeight; ++y) {
            const uint8_t* src = inFrame.data[1] + static_cast<std::ptrdiff_t>(y) * inFrame.linesize[1];
            uint8_t* dstU = outFrame.data[1] + static_cast<std::ptrdiff_t>(y) * outFrame.linesize[1];
            uint8_t* dstV = outFrame.data[2] + static_cast<std::ptrdiff_t>(y) * outFrame.linesize[2];
            for (int x = 0; x < chromaWidth; ++x) {
                dstU[x] = src[x * 2];
                dstV[x] = src[x * 2 + 1];
            }
        }
        return true;
    }
    if (inputPixelFormat == AV_PIX_FMT_YUV420P && outputPixelFormat == AV_PIX_FMT_NV12) {
        if (inFrame.data[1] == nullptr || inFrame.data[2] == nullptr || outFrame.data[1] == nullptr) {
            return false;
        }
        for (int y = 0; y < chromaHeight; ++y) {
            const uint8_t* srcU = inFrame.data[1] + static_cast<std::ptrdiff_t>(y) * inFrame.linesize[1];
            const uint8_t* srcV = inFrame.data[2] + static_cast<std::ptrdiff_t>(y) * inFrame.linesize[2];
            uint8_t* dst = outFrame.data[1] + static_cast<std::ptrdiff_t>(y) * outFrame.linesize[1];
            for (int x = 0; x < chromaWidth; ++x) {
                dst[x * 2] = srcU[x];
                dst[x * 2 + 1] = srcV[x];
            }
        }
        return true;
    }

    return false;
}

}  // namespace

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::configure(int width,
                             int height,
                             int frameRate,
                             int bitrate,
                             uint8_t payloadType,
                             VideoEncoderPreset preset) {
    if (width <= 0 || height <= 0 || frameRate <= 0 || bitrate <= 0) {
        return false;
    }
    if (payloadType == 0) {
        payloadType = kScreenSharePayloadType;
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

            applyLowLatencyCodecOptions(candidate, context.get(), preset);
            av_opt_set(context->priv_data, "repeat_headers", "1", 0);

            if (avcodec_open2(context.get(), codec, nullptr) < 0) {
                continue;
            }

            m_width = width;
            m_height = height;
            m_frameRate = frameRate;
            m_bitrate = bitrate;
            m_payloadType = payloadType;
            m_preset = preset;
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
        if (!configure(inFrame.width, inFrame.height, 5, 1500 * 1000, m_payloadType, m_preset)) {
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
    const bool requestKeyFrame = consumeKeyframeRequest(forceKeyFrame);
    if (requestKeyFrame) {
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

    return receivePacket(inFrame.pts, outFrame, error);
}

bool VideoEncoder::encode(const AVFrame& inFrame,
                          EncodedVideoFrame& outFrame,
                          bool forceKeyFrame,
                          std::string* error) {
    const int inputWidth = inFrame.width;
    const int inputHeight = inFrame.height;
    if (inputWidth <= 0 || inputHeight <= 0) {
        if (error != nullptr) {
            *error = "invalid input frame size";
        }
        return false;
    }

    if (!m_codecContext) {
        const int configuredFrameRate = m_frameRate > 0 ? m_frameRate : 30;
        const int configuredBitrate = m_bitrate > 0 ? m_bitrate : 1500 * 1000;
        if (!configure(inputWidth, inputHeight, configuredFrameRate, configuredBitrate, m_payloadType, m_preset)) {
            if (error != nullptr) {
                *error = "encoder configure failed";
            }
            return false;
        }
    }

    if (inputWidth != m_width || inputHeight != m_height) {
        if (error != nullptr) {
            *error = "input size mismatch";
        }
        return false;
    }

    const AVPixelFormat inputPixelFormat = static_cast<AVPixelFormat>(inFrame.format);
    av::AVFramePtr frame = nullptr;
    if (inputPixelFormat == m_outputPixelFormat) {
        frame.reset(av_frame_clone(&inFrame));
    } else {
        frame = av::makeFrame();
        if (frame) {
            frame->format = static_cast<int>(m_outputPixelFormat);
            frame->width = m_width;
            frame->height = m_height;
            frame->pts = inFrame.pts;
            const bool allocateOk = av_frame_get_buffer(frame.get(), 32) >= 0;
            const bool convertOk = inputPixelFormat == AV_PIX_FMT_BGRA
                ? fillFrameFromBgraInput(inFrame, *frame, m_outputPixelFormat)
                : fillFrameFromYuvInput(inFrame, *frame, m_outputPixelFormat);
            if (!allocateOk || !convertOk) {
                frame.reset();
            }
        }
    }
    if (!frame) {
        if (error != nullptr) {
            *error = "input pixel format mismatch";
        }
        return false;
    }

    const bool requestKeyFrame = consumeKeyframeRequest(forceKeyFrame);
    if (requestKeyFrame) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->key_frame = 1;
    }

    const int sendResult = avcodec_send_frame(m_codecContext.get(), frame.get());
    if (sendResult < 0) {
        if (error != nullptr) {
            *error = "avcodec_send_frame failed: " + describeAvError(sendResult);
        }
        return false;
    }

    return receivePacket(inFrame.pts, outFrame, error);
}

bool VideoEncoder::receivePacket(int64_t fallbackPts, EncodedVideoFrame& outFrame, std::string* error) {
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
    outFrame.pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : fallbackPts;
    outFrame.keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    outFrame.payloadType = m_payloadType;
    outFrame.payload.assign(packet->data, packet->data + packet->size);
    if (outFrame.payload.empty()) {
        if (error != nullptr) {
            *error = "empty video packet";
        }
        return false;
    }

    const int nalLengthSize = (m_codecContext->extradata != nullptr && m_codecContext->extradata_size >= 5 &&
                               m_codecContext->extradata[0] == 1)
                                  ? ((m_codecContext->extradata[4] & 0x03) + 1)
                                  : 4;
    std::vector<uint8_t> annexBPayload;
    if (!convertAvccPayloadToAnnexB(outFrame.payload, nalLengthSize, annexBPayload)) {
        if (error != nullptr) {
            *error = "avcc-to-annexb conversion failed";
        }
        return false;
    }
    prependParameterSetsForKeyFrame(annexBPayload,
                                    outFrame.keyFrame,
                                    m_codecContext->extradata,
                                    m_codecContext->extradata_size);
    outFrame.payload = std::move(annexBPayload);

    return true;
}

bool VideoEncoder::setBitrate(int bitrate) {
    if (bitrate <= 0) {
        return false;
    }

    m_bitrate = bitrate;
    if (m_codecContext) {
        m_codecContext->bit_rate = bitrate;
        if (m_codecContext->priv_data != nullptr) {
            av_opt_set_int(m_codecContext->priv_data, "b", bitrate, 0);
            av_opt_set_int(m_codecContext->priv_data, "bitrate", bitrate, 0);
        }
    }
    return true;
}

bool VideoEncoder::setPreset(VideoEncoderPreset preset) {
    if (!m_codecContext) {
        m_preset = preset;
        return true;
    }
    if (preset == m_preset) {
        return true;
    }
    return configure(m_width, m_height, m_frameRate, m_bitrate, m_payloadType, preset);
}

bool VideoEncoder::setPayloadType(uint8_t payloadType) {
    if (payloadType == 0) {
        return false;
    }
    m_payloadType = payloadType;
    return true;
}

bool VideoEncoder::requestKeyframe() {
    m_forceKeyFrameNext = true;
    return true;
}

bool VideoEncoder::consumeKeyframeRequest(bool forceKeyFrame) {
    const bool requested = forceKeyFrame || m_forceKeyFrameNext;
    m_forceKeyFrameNext = false;
    return requested;
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

VideoEncoderPreset VideoEncoder::preset() const {
    return m_preset;
}

uint8_t VideoEncoder::payloadType() const {
    return m_payloadType;
}

}  // namespace av::codec

