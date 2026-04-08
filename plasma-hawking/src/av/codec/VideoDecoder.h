#pragma once

#include "av/FFmpegUtils.h"
#include "av/codec/VideoEncoder.h"

#include <cstdint>
#include <string>
#include <vector>

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
    av::AVCodecContextPtr m_codecContext;
};

}  // namespace av::codec
