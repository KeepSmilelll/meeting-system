#pragma once

#include "av/capture/ScreenCapture.h"
#include "av/codec/VideoEncoder.h"
#include "net/media/RTPSender.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace av::session {

struct VideoSendPipelineConfig {
    int frameRate{5};
    std::size_t maxPayloadBytes{1200};
};

struct VideoSendPipelinePacket {
    uint16_t sequenceNumber{0};
    uint32_t timestamp{0};
    std::vector<uint8_t> bytes;
};

struct VideoSendPipelineInputFrame {
    av::capture::ScreenFrame screenFrame;
    av::AVFramePtr avFrame{nullptr};

    bool hasAvFrame() const {
        return static_cast<bool>(avFrame);
    }

    bool hasScreenFrame() const {
        return !screenFrame.bgra.empty();
    }
};

class VideoSendPipeline {
public:
    explicit VideoSendPipeline(VideoSendPipelineConfig config = {});

    bool encodeAndPacketize(av::codec::VideoEncoder& encoder,
                            const VideoSendPipelineInputFrame& inputFrame,
                            uint8_t payloadType,
                            bool forceKeyFrame,
                            media::RTPSender& sender,
                            std::vector<VideoSendPipelinePacket>& outPackets,
                            bool* encodedKeyFrame = nullptr,
                            std::string* error = nullptr) const;

    bool encodeAndPacketize(av::codec::VideoEncoder& encoder,
                            const av::capture::ScreenFrame& frame,
                            uint8_t payloadType,
                            bool forceKeyFrame,
                            media::RTPSender& sender,
                            std::vector<VideoSendPipelinePacket>& outPackets,
                            bool* encodedKeyFrame = nullptr,
                            std::string* error = nullptr) const;

    bool encodeAndPacketize(av::codec::VideoEncoder& encoder,
                            const AVFrame& frame,
                            uint8_t payloadType,
                            bool forceKeyFrame,
                            media::RTPSender& sender,
                            std::vector<VideoSendPipelinePacket>& outPackets,
                            bool* encodedKeyFrame = nullptr,
                            std::string* error = nullptr) const;

private:
    VideoSendPipelineConfig m_config;
};

}  // namespace av::session
