#pragma once

#include "av/FFmpegUtils.h"
#include "av/capture/AudioCapture.h"

#include <cstdint>
#include <vector>

namespace av::codec {

struct EncodedAudioFrame {
    int sampleRate{48000};
    int channels{1};
    int64_t pts{0};
    uint8_t payloadType{111};  // Opus RTP PT
    std::vector<uint8_t> payload;
};

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    bool configure(int sampleRate, int channels, int bitrate);
    bool encode(const capture::AudioFrame& inFrame, EncodedAudioFrame& outFrame) const;

    int sampleRate() const;
    int channels() const;
    int bitrate() const;

private:
    int m_sampleRate{48000};
    int m_channels{1};
    int m_bitrate{32000};
    AVChannelLayout m_channelLayout{};
};

}  // namespace av::codec
