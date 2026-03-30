#pragma once

#include "av/FFmpegUtils.h"
#include "av/capture/AudioCapture.h"
#include "AudioEncoder.h"

namespace av::codec {

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    bool configure(int sampleRate, int channels);
    bool decode(const EncodedAudioFrame& inFrame, capture::AudioFrame& outFrame) const;

    int sampleRate() const;
    int channels() const;

private:
    int m_sampleRate{48000};
    int m_channels{1};
    AVChannelLayout m_channelLayout{};
};

// Minimal callable self-check for
// capture -> encode -> rtp send/recv -> decode.
bool runAudioPipelineLoopbackSelfCheck();

}  // namespace av::codec
