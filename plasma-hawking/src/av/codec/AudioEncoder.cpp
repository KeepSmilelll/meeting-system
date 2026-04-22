#include "AudioEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/opt.h>
}

namespace av::codec {
namespace {

constexpr int kOpusFrameDurationMs = 20;
constexpr uint8_t kOpusPayloadType = 111;

const AVCodec* findOpusEncoder() {
    if (const AVCodec* codec = avcodec_find_encoder_by_name("libopus")) {
        return codec;
    }
    return avcodec_find_encoder(AV_CODEC_ID_OPUS);
}

AVSampleFormat chooseOpusSampleFormat(const AVCodec* codec) {
    static constexpr AVSampleFormat kPreferredFormats[] = {
        AV_SAMPLE_FMT_FLTP,
        AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_S16,
    };

    auto isSupported = [codec](AVSampleFormat fmt) {
        if (!codec || !codec->sample_fmts) {
            return true;
        }
        for (const AVSampleFormat* supported = codec->sample_fmts; *supported != AV_SAMPLE_FMT_NONE; ++supported) {
            if (*supported == fmt) {
                return true;
            }
        }
        return false;
    };

    for (AVSampleFormat fmt : kPreferredFormats) {
        if (isSupported(fmt)) {
            return fmt;
        }
    }

    if (codec && codec->sample_fmts && codec->sample_fmts[0] != AV_SAMPLE_FMT_NONE) {
        return codec->sample_fmts[0];
    }

    return AV_SAMPLE_FMT_FLTP;
}

int frameSamplesFor20ms(int sampleRate) {
    return (sampleRate * kOpusFrameDurationMs) / 1000;
}

std::string describeAvError(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return buffer;
}

int16_t floatToPcm16(float sample) {
    const float clamped = std::clamp(sample, -1.0F, 1.0F);
    return static_cast<int16_t>(std::lrint(clamped * 32767.0F));
}

bool writeSamplesToFrame(const capture::AudioFrame& inFrame, AVFrame& frame) {
    const int channels = inFrame.channels;
    if (channels <= 0) {
        return false;
    }

    const int totalSamples = static_cast<int>(inFrame.samples.size());
    if (totalSamples <= 0 || (totalSamples % channels) != 0) {
        return false;
    }

    const int samplesPerChannel = totalSamples / channels;
    if (samplesPerChannel != frame.nb_samples) {
        return false;
    }

    if (av_frame_make_writable(&frame) < 0) {
        return false;
    }

    auto* const* planes = frame.extended_data ? frame.extended_data : frame.data;
    switch (static_cast<AVSampleFormat>(frame.format)) {
    case AV_SAMPLE_FMT_FLTP: {
        for (int ch = 0; ch < channels; ++ch) {
            auto* dst = reinterpret_cast<float*>(planes[ch]);
            for (int i = 0; i < samplesPerChannel; ++i) {
                dst[i] = inFrame.samples[i * channels + ch];
            }
        }
        return true;
    }
    case AV_SAMPLE_FMT_FLT: {
        auto* dst = reinterpret_cast<float*>(planes[0]);
        for (int i = 0; i < samplesPerChannel; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                dst[i * channels + ch] = inFrame.samples[i * channels + ch];
            }
        }
        return true;
    }
    case AV_SAMPLE_FMT_S16P: {
        for (int ch = 0; ch < channels; ++ch) {
            auto* dst = reinterpret_cast<int16_t*>(planes[ch]);
            for (int i = 0; i < samplesPerChannel; ++i) {
                dst[i] = floatToPcm16(inFrame.samples[i * channels + ch]);
            }
        }
        return true;
    }
    case AV_SAMPLE_FMT_S16: {
        auto* dst = reinterpret_cast<int16_t*>(planes[0]);
        for (int i = 0; i < samplesPerChannel; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                dst[i * channels + ch] = floatToPcm16(inFrame.samples[i * channels + ch]);
            }
        }
        return true;
    }
    default:
        return false;
    }
}

}  // namespace
AudioEncoder::AudioEncoder() {
    setDefaultChannelLayout(m_channelLayout, m_channels);
}

AudioEncoder::~AudioEncoder() {
    av_channel_layout_uninit(&m_channelLayout);
}

bool AudioEncoder::configure(int sampleRate, int channels, int bitrate) {
    if (sampleRate != kDefaultSampleRate || bitrate <= 0 || (channels != 1 && channels != 2)) {
        return false;
    }

    const AVCodec* codec = findOpusEncoder();
    if (!codec) {
        return false;
    }

    AVChannelLayout newLayout{};
    if (!setDefaultChannelLayout(newLayout, channels)) {
        return false;
    }

    const AVSampleFormat selectedSampleFormat = chooseOpusSampleFormat(codec);
    if (selectedSampleFormat == AV_SAMPLE_FMT_NONE) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }

    AVCodecContextPtr newContext = makeCodecContext(codec);
    if (!newContext) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }

    newContext->bit_rate = bitrate;
    newContext->sample_rate = sampleRate;
    newContext->time_base = AVRational{1, sampleRate};
    newContext->sample_fmt = selectedSampleFormat;
    if (av_channel_layout_copy(&newContext->ch_layout, &newLayout) < 0) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }

    AVDictionary* codecOptions = nullptr;
    av_dict_set(&codecOptions, "application", "voip", 0);
    av_dict_set(&codecOptions, "frame_duration", "20.0", 0);
    av_dict_set(&codecOptions, "vbr", "constrained", 0);

    if (codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) {
        newContext->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }

    if (avcodec_open2(newContext.get(), codec, &codecOptions) < 0) {
        av_dict_free(&codecOptions);
        av_channel_layout_uninit(&newLayout);
        return false;
    }
    av_dict_free(&codecOptions);

    av_channel_layout_uninit(&m_channelLayout);
    if (av_channel_layout_copy(&m_channelLayout, &newLayout) < 0) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }
    av_channel_layout_uninit(&newLayout);

    m_codecContext = std::move(newContext);
    m_codecSampleFormat = selectedSampleFormat;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitrate = bitrate;
    m_frameSamples = m_codecContext->frame_size > 0 ? m_codecContext->frame_size : frameSamplesFor20ms(sampleRate);
    return true;
}

bool AudioEncoder::encode(const capture::AudioFrame& inFrame, EncodedAudioFrame& outFrame, std::string* error) const {
    if (!m_codecContext) {
        auto* self = const_cast<AudioEncoder*>(this);
        if (!self->configure(m_sampleRate, m_channels, m_bitrate)) {
            if (error != nullptr) {
                *error = "encoder configure failed";
            }
            return false;
        }
    }

    if (inFrame.sampleRate != m_sampleRate || inFrame.channels != m_channels) {
        if (error != nullptr) {
            *error = "input frame metadata mismatch";
        }
        return false;
    }

    const int totalSamples = static_cast<int>(inFrame.samples.size());
    if (totalSamples <= 0 || (totalSamples % m_channels) != 0) {
        if (error != nullptr) {
            *error = "invalid input sample count";
        }
        return false;
    }

    const int samplesPerChannel = totalSamples / m_channels;
    if (samplesPerChannel != m_frameSamples) {
        if (error != nullptr) {
            *error = "unexpected frame size expected=" + std::to_string(m_frameSamples) + " got=" + std::to_string(samplesPerChannel);
        }
        return false;
    }

    AVFramePtr inputFrame = makeFrame();
    if (!inputFrame) {
        if (error != nullptr) {
            *error = "failed to allocate audio frame";
        }
        return false;
    }

    if (!configureAudioFrame(*inputFrame, m_sampleRate, m_channelLayout, m_codecSampleFormat, samplesPerChannel)) {
        if (error != nullptr) {
            *error = "failed to configure audio frame";
        }
        return false;
    }

    inputFrame->pts = inFrame.pts;
    if (!writeSamplesToFrame(inFrame, *inputFrame)) {
        if (error != nullptr) {
            *error = "failed to copy PCM samples";
        }
        return false;
    }

    const int sendResult = avcodec_send_frame(m_codecContext.get(), inputFrame.get());
    if (sendResult < 0) {
        if (error != nullptr) {
            *error = "avcodec_send_frame failed: " + describeAvError(sendResult) +
                     " codec_frame_size=" + std::to_string(m_codecContext->frame_size) +
                     " sample_fmt=" + std::to_string(static_cast<int>(m_codecContext->sample_fmt));
        }
        return false;
    }

    AVPacketPtr packet = makePacket();
    if (!packet) {
        if (error != nullptr) {
            *error = "failed to allocate packet";
        }
        return false;
    }

    const int receiveResult = avcodec_receive_packet(m_codecContext.get(), packet.get());
    if (receiveResult < 0) {
        if (error != nullptr) {
            *error = "avcodec_receive_packet failed: " + describeAvError(receiveResult);
        }
        return false;
    }

    outFrame.sampleRate = m_sampleRate;
    outFrame.channels = m_channels;
    outFrame.pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : inFrame.pts;
    outFrame.frameSamples = packet->duration > 0 ? static_cast<int>(packet->duration) : frameSamplesFor20ms(m_sampleRate);
    outFrame.payloadType = kOpusPayloadType;
    outFrame.payload.assign(packet->data, packet->data + packet->size);
    if (outFrame.payload.empty()) {
        if (error != nullptr) {
            *error = "encoder produced empty opus packet";
        }
        return false;
    }
    return true;
}

bool AudioEncoder::setBitrate(int bitrate) {
    if (bitrate <= 0) {
        return false;
    }

    m_bitrate = bitrate;
    if (m_codecContext) {
        m_codecContext->bit_rate = bitrate;
        if (m_codecContext->priv_data != nullptr) {
            av_opt_set_int(m_codecContext->priv_data, "b", bitrate, 0);
            av_opt_set_int(m_codecContext->priv_data, "bitrate", bitrate, 0);
        }
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

int AudioEncoder::frameSamples() const {
    return m_frameSamples;
}

}  // namespace av::codec









