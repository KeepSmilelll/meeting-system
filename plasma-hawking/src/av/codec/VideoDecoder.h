#pragma once

#include "av/FFmpegUtils.h"
#include "av/codec/VideoEncoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVBufferRef;

namespace av::codec {

struct DecodedVideoFrame {
    int width{0};
    int height{0};
    int64_t pts{0};
    std::vector<uint8_t> yPlane;
    std::vector<uint8_t> uvPlane;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool configure();
    bool decode(const EncodedVideoFrame& inFrame, DecodedVideoFrame& outFrame, std::string* error = nullptr);

private:
    struct AvBufferRefDeleter {
        void operator()(AVBufferRef* ref) const;
    };

    using AvBufferRefPtr = std::unique_ptr<AVBufferRef, AvBufferRefDeleter>;

    static AVPixelFormat selectPixelFormat(AVCodecContext* context, const AVPixelFormat* formats);
    bool configureHardwareDecode(const AVCodec* codec, AVCodecContext& context);

    av::AVCodecContextPtr m_codecContext;
    AvBufferRefPtr m_hwDeviceContext;
    AVPixelFormat m_hwPixelFormat{AV_PIX_FMT_NONE};
};

}  // namespace av::codec
