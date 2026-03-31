#pragma once

#include "av/FFmpegUtils.h"
#include "av/capture/AudioCapture.h"
#include "AudioEncoder.h"

#include <string>

namespace av::codec {

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    bool configure(int sampleRate, int channels);
    bool decode(const EncodedAudioFrame& inFrame, capture::AudioFrame& outFrame, std::string* error = nullptr) const;

    int sampleRate() const;
    int channels() const;

private:
    static constexpr int kDefaultSampleRate = 48000;
    static constexpr int kDefaultChannels = 1;

    int m_sampleRate{kDefaultSampleRate};
    int m_channels{kDefaultChannels};
    AVChannelLayout m_channelLayout{};
    mutable av::AVCodecContextPtr m_codecContext;
};

// Minimal callable self-check for
// capture -> encode -> rtp send/recv -> decode.
bool runAudioPipelineLoopbackSelfCheck(std::string* error = nullptr);

}  // namespace av::codec
