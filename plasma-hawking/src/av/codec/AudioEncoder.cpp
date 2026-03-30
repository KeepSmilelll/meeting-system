#include "AudioEncoder.h"

#include <algorithm>
#include <cmath>

namespace av::codec {
namespace {

inline int16_t floatToPcm16(float sample) {
    const float clamped = std::max(-1.0F, std::min(1.0F, sample));
    const float scaled = clamped * 32767.0F;
    return static_cast<int16_t>(std::lrint(scaled));
}

}  // namespace

AudioEncoder::AudioEncoder() {
    setDefaultChannelLayout(m_channelLayout, m_channels);
}

AudioEncoder::~AudioEncoder() {
    av_channel_layout_uninit(&m_channelLayout);
}

bool AudioEncoder::configure(int sampleRate, int channels, int bitrate) {
    if (sampleRate <= 0 || channels <= 0 || bitrate <= 0) {
        return false;
    }

    AVChannelLayout newLayout{};
    if (!setDefaultChannelLayout(newLayout, channels)) {
        return false;
    }

    av_channel_layout_uninit(&m_channelLayout);
    if (av_channel_layout_copy(&m_channelLayout, &newLayout) != 0) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }
    av_channel_layout_uninit(&newLayout);
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitrate = bitrate;
    return true;
}

bool AudioEncoder::encode(const capture::AudioFrame& inFrame, EncodedAudioFrame& outFrame) const {
    if (inFrame.sampleRate != m_sampleRate || inFrame.channels != m_channels) {
        return false;
    }

    outFrame.sampleRate = inFrame.sampleRate;
    outFrame.channels = inFrame.channels;
    outFrame.pts = inFrame.pts;
    outFrame.payloadType = 111;
    outFrame.payload.resize(inFrame.samples.size() * sizeof(int16_t));

    for (std::size_t i = 0; i < inFrame.samples.size(); ++i) {
        const int16_t pcm = floatToPcm16(inFrame.samples[i]);
        outFrame.payload[i * 2] = static_cast<uint8_t>(pcm & 0xFF);
        outFrame.payload[i * 2 + 1] = static_cast<uint8_t>((pcm >> 8) & 0xFF);
    }

    return true;
}

int AudioEncoder::sampleRate() const {
    return m_sampleRate;
}

int AudioEncoder::channels() const {
    return m_channels;
}

int AudioEncoder::bitrate() const {
    return m_bitrate;
}

}  // namespace av::codec

