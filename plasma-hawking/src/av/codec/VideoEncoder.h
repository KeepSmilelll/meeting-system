#pragma once

#include "av/FFmpegUtils.h"
#include "av/capture/ScreenCapture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SwsContext;

namespace av::codec {

struct EncodedVideoFrame {
    int width{0};
    int height{0};
    int frameRate{0};
    int64_t pts{0};
    bool keyFrame{false};
    uint8_t payloadType{97};
    std::vector<uint8_t> payload;
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    bool configure(int width, int height, int frameRate, int bitrate);
    bool encode(const capture::ScreenFrame& inFrame,
                EncodedVideoFrame& outFrame,
                bool forceKeyFrame = false,
                std::string* error = nullptr);

    int width() const;
    int height() const;
    int frameRate() const;
    int bitrate() const;
    uint8_t payloadType() const;

private:
    struct SwsContextDeleter {
        void operator()(SwsContext* ctx) const;
    };

    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

    int m_width{0};
    int m_height{0};
    int m_frameRate{0};
    int m_bitrate{0};
    uint8_t m_payloadType{97};
    AVPixelFormat m_outputPixelFormat{AV_PIX_FMT_NONE};
    av::AVCodecContextPtr m_codecContext;
    SwsContextPtr m_swsContext;
    std::string m_codecName;
};

}  // namespace av::codec
