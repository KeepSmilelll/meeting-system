#pragma once

#include "av/FFmpegUtils.h"
#include "av/capture/AudioCapture.h"

#include <cstdint>
#include <string>
#include <vector>

namespace av::codec {

struct EncodedAudioFrame {
    int sampleRate{48000};
    int channels{1};
    int64_t pts{0};
    int frameSamples{960};
    uint8_t payloadType{111};  // Opus RTP PT
    std::vector<uint8_t> payload;
};

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    bool configure(int sampleRate, int channels, int bitrate);
    bool encode(const capture::AudioFrame& inFrame, EncodedAudioFrame& outFrame, std::string* error = nullptr) const;

    int sampleRate() const;
    int channels() const;
    int bitrate() const;
    int frameSamples() const;

private:
    static constexpr int kDefaultSampleRate = 48000;
    static constexpr int kDefaultChannels = 1;
    static constexpr int kDefaultBitrate = 32000;

    int m_sampleRate{kDefaultSampleRate};
    int m_channels{kDefaultChannels};
    int m_bitrate{kDefaultBitrate};
    int m_frameSamples{960};
    AVChannelLayout m_channelLayout{};
    mutable av::AVCodecContextPtr m_codecContext;
    mutable AVSampleFormat m_codecSampleFormat{AV_SAMPLE_FMT_NONE};
};

}  // namespace av::codec
