#pragma once

#include "av/FFmpegUtils.h"
#include "av/capture/ScreenCapture.h"

#include <cstdint>
#include <string>
#include <vector>

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

enum class VideoEncoderPreset {
    Realtime,
    Balanced,
    Quality,
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    bool configure(int width,
                   int height,
                   int frameRate,
                   int bitrate,
                   uint8_t payloadType = 97,
                   VideoEncoderPreset preset = VideoEncoderPreset::Realtime);
    bool encode(const capture::ScreenFrame& inFrame,
                EncodedVideoFrame& outFrame,
                bool forceKeyFrame = false,
                std::string* error = nullptr);
    bool encode(const AVFrame& inFrame,
                EncodedVideoFrame& outFrame,
                bool forceKeyFrame = false,
                std::string* error = nullptr);
    bool setBitrate(int bitrate);
    bool setPreset(VideoEncoderPreset preset);
    bool setPayloadType(uint8_t payloadType);
    bool requestKeyframe();

    int width() const;
    int height() const;
    int frameRate() const;
    int bitrate() const;
    VideoEncoderPreset preset() const;
    uint8_t payloadType() const;

private:
    bool receivePacket(int64_t fallbackPts, EncodedVideoFrame& outFrame, std::string* error);
    bool consumeKeyframeRequest(bool forceKeyFrame);

    int m_width{0};
    int m_height{0};
    int m_frameRate{0};
    int m_bitrate{0};
    uint8_t m_payloadType{97};
    VideoEncoderPreset m_preset{VideoEncoderPreset::Realtime};
    AVPixelFormat m_outputPixelFormat{AV_PIX_FMT_NONE};
    av::AVCodecContextPtr m_codecContext;
    std::string m_codecName;
    bool m_forceKeyFrameNext{false};
};

}  // namespace av::codec
