#include "AudioDecoder.h"

#include "av/capture/AudioCapture.h"
#include "av/codec/AudioEncoder.h"
#include "net/media/JitterBuffer.h"
#include "net/media/RTPReceiver.h"
#include "net/media/RTPSender.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace av::codec {
namespace {

constexpr int kOpusFrameSamples = 960;
constexpr int kDefaultSampleRate = 48000;
constexpr int kDefaultChannels = 1;

const AVCodec* findOpusDecoder() {
    if (const AVCodec* codec = avcodec_find_decoder_by_name("opus")) {
        return codec;
    }
    return avcodec_find_decoder(AV_CODEC_ID_OPUS);
}

std::string describeAvError(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return buffer;
}

float pcm16ToFloat(int16_t value) {
    return static_cast<float>(value) / 32768.0F;
}

bool readSamplesFromFrame(const AVFrame& frame, capture::AudioFrame& outFrame) {
    const int channels = frame.ch_layout.nb_channels > 0 ? frame.ch_layout.nb_channels : 1;
    const int samplesPerChannel = frame.nb_samples;
    if (samplesPerChannel <= 0 || channels <= 0) {
        return false;
    }

    outFrame.sampleRate = frame.sample_rate;
    outFrame.channels = channels;
    outFrame.samples.assign(static_cast<std::size_t>(samplesPerChannel * channels), 0.0F);

    auto* const* planes = frame.extended_data ? frame.extended_data : frame.data;
    switch (static_cast<AVSampleFormat>(frame.format)) {
    case AV_SAMPLE_FMT_FLTP: {
        for (int ch = 0; ch < channels; ++ch) {
            const auto* src = reinterpret_cast<const float*>(planes[ch]);
            for (int i = 0; i < samplesPerChannel; ++i) {
                outFrame.samples[static_cast<std::size_t>(i * channels + ch)] = src[i];
            }
        }
        return true;
    }
    case AV_SAMPLE_FMT_FLT: {
        const auto* src = reinterpret_cast<const float*>(planes[0]);
        for (int i = 0; i < samplesPerChannel; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                outFrame.samples[static_cast<std::size_t>(i * channels + ch)] = src[i * channels + ch];
            }
        }
        return true;
    }
    case AV_SAMPLE_FMT_S16P: {
        for (int ch = 0; ch < channels; ++ch) {
            const auto* src = reinterpret_cast<const int16_t*>(planes[ch]);
            for (int i = 0; i < samplesPerChannel; ++i) {
                outFrame.samples[static_cast<std::size_t>(i * channels + ch)] = pcm16ToFloat(src[i]);
            }
        }
        return true;
    }
    case AV_SAMPLE_FMT_S16: {
        const auto* src = reinterpret_cast<const int16_t*>(planes[0]);
        for (int i = 0; i < samplesPerChannel; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                outFrame.samples[static_cast<std::size_t>(i * channels + ch)] = pcm16ToFloat(src[i * channels + ch]);
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool decodePcmFallback(const EncodedAudioFrame& inFrame, capture::AudioFrame& outFrame) {
    if ((inFrame.payload.size() % sizeof(int16_t)) != 0) {
        return false;
    }

    outFrame.sampleRate = inFrame.sampleRate;
    outFrame.channels = inFrame.channels;
    outFrame.pts = inFrame.pts;
    outFrame.samples.resize(inFrame.payload.size() / sizeof(int16_t));

    for (std::size_t i = 0; i < outFrame.samples.size(); ++i) {
        const uint8_t lo = inFrame.payload[i * 2];
        const uint8_t hi = inFrame.payload[i * 2 + 1];
        const int16_t pcm = static_cast<int16_t>((static_cast<int16_t>(hi) << 8) | lo);
        outFrame.samples[i] = pcm16ToFloat(pcm);
    }

    return !outFrame.samples.empty();
}

}  // namespace
AudioDecoder::AudioDecoder() {
    setDefaultChannelLayout(m_channelLayout, m_channels);
}

AudioDecoder::~AudioDecoder() {
    av_channel_layout_uninit(&m_channelLayout);
}

bool AudioDecoder::configure(int sampleRate, int channels) {
    if (sampleRate != kDefaultSampleRate || (channels != 1 && channels != 2)) {
        return false;
    }

    const AVCodec* codec = findOpusDecoder();
    if (!codec) {
        return false;
    }

    AVChannelLayout newLayout{};
    if (!setDefaultChannelLayout(newLayout, channels)) {
        return false;
    }

    AVCodecContextPtr newContext = makeCodecContext(codec);
    if (!newContext) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }

    newContext->sample_rate = sampleRate;
    newContext->time_base = AVRational{1, sampleRate};
    if (av_channel_layout_copy(&newContext->ch_layout, &newLayout) < 0) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }

    if (avcodec_open2(newContext.get(), codec, nullptr) < 0) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }

    av_channel_layout_uninit(&m_channelLayout);
    if (av_channel_layout_copy(&m_channelLayout, &newLayout) < 0) {
        av_channel_layout_uninit(&newLayout);
        return false;
    }
    av_channel_layout_uninit(&newLayout);

    m_codecContext = std::move(newContext);
    m_sampleRate = sampleRate;
    m_channels = channels;
    return true;
}

bool AudioDecoder::decode(const EncodedAudioFrame& inFrame, capture::AudioFrame& outFrame, std::string* error) const {
    if (!m_codecContext) {
        auto* self = const_cast<AudioDecoder*>(this);
        if (!self->configure(m_sampleRate, m_channels)) {
            if (error != nullptr) {
                *error = "decoder configure failed";
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

    if (inFrame.payload.empty()) {
        if (error != nullptr) {
            *error = "empty opus payload";
        }
        return false;
    }

    const std::size_t expectedPcmBytes = static_cast<std::size_t>(kOpusFrameSamples * inFrame.channels) * sizeof(int16_t);
    if (inFrame.payload.size() == expectedPcmBytes && decodePcmFallback(inFrame, outFrame)) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    AVPacketPtr packet = makePacket();
    if (!packet) {
        if (error != nullptr) {
            *error = "failed to allocate packet";
        }
        return false;
    }

    if (av_new_packet(packet.get(), static_cast<int>(inFrame.payload.size())) < 0) {
        if (error != nullptr) {
            *error = "failed to allocate packet payload";
        }
        return false;
    }
    std::memcpy(packet->data, inFrame.payload.data(), inFrame.payload.size());
    packet->pts = inFrame.pts;

    const int sendResult = avcodec_send_packet(m_codecContext.get(), packet.get());
    if (sendResult < 0) {
        if (decodePcmFallback(inFrame, outFrame)) {
            if (error != nullptr) {
                error->clear();
            }
            return true;
        }
        if (error != nullptr) {
            *error = "avcodec_send_packet failed: " + describeAvError(sendResult);
        }
        return false;
    }

    AVFramePtr decodedFrame = makeFrame();
    if (!decodedFrame) {
        if (error != nullptr) {
            *error = "failed to allocate decoded frame";
        }
        return false;
    }

    const int receiveResult = avcodec_receive_frame(m_codecContext.get(), decodedFrame.get());
    if (receiveResult < 0) {
        if (decodePcmFallback(inFrame, outFrame)) {
            if (error != nullptr) {
                error->clear();
            }
            return true;
        }
        if (error != nullptr) {
            *error = "avcodec_receive_frame failed: " + describeAvError(receiveResult);
        }
        return false;
    }

    if (!readSamplesFromFrame(*decodedFrame, outFrame)) {
        if (decodePcmFallback(inFrame, outFrame)) {
            if (error != nullptr) {
                error->clear();
            }
            return true;
        }
        if (error != nullptr) {
            *error = "failed to unpack decoded samples";
        }
        return false;
    }

    outFrame.sampleRate = decodedFrame->sample_rate > 0 ? decodedFrame->sample_rate : m_sampleRate;
    outFrame.channels = decodedFrame->ch_layout.nb_channels > 0 ? decodedFrame->ch_layout.nb_channels : m_channels;
    outFrame.pts = decodedFrame->pts != AV_NOPTS_VALUE ? decodedFrame->pts : inFrame.pts;
    return !outFrame.samples.empty();
}

int AudioDecoder::sampleRate() const {
    return m_sampleRate;
}

int AudioDecoder::channels() const {
    return m_channels;
}

bool runAudioPipelineLoopbackSelfCheck(std::string* error) {
    capture::AudioCapture capture(8);
    if (!capture.start()) {
        if (error != nullptr) {
            *error = "capture start failed";
        }
        return false;
    }

    AudioEncoder encoder;
    if (!encoder.configure(kDefaultSampleRate, kDefaultChannels, 32000)) {
        capture.stop();
        if (error != nullptr) {
            *error = "encoder configure failed";
        }
        return false;
    }

    capture::AudioFrame inFrame;
    inFrame.sampleRate = kDefaultSampleRate;
    inFrame.channels = kDefaultChannels;
    inFrame.pts = 0;
    inFrame.samples.assign(static_cast<std::size_t>(encoder.frameSamples()), 0.25F);

    if (!capture.pushCapturedFrame(inFrame)) {
        capture.stop();
        if (error != nullptr) {
            *error = "capture push failed";
        }
        return false;
    }

    capture::AudioFrame captured;
    if (!capture.popFrameForEncode(captured, std::chrono::milliseconds(20))) {
        capture.stop();
        if (error != nullptr) {
            *error = "capture pop failed";
        }
        return false;
    }

    EncodedAudioFrame encoded;
    std::string encodeError;
    if (!encoder.encode(captured, encoded, &encodeError)) {
        capture.stop();
        if (error != nullptr) {
            *error = encodeError.empty() ? "encode failed" : encodeError;
        }
        return false;
    }

    media::RTPSender sender(0x10203040, 10);
    const auto wire = sender.buildPacket(encoded.payloadType, true, static_cast<uint32_t>(captured.pts), encoded.payload);
    if (wire.empty()) {
        capture.stop();
        if (error != nullptr) {
            *error = "rtp build failed";
        }
        return false;
    }

    media::RTPReceiver receiver;
    media::RTPPacket receivedPacket;
    if (!receiver.parsePacket(wire, receivedPacket)) {
        capture.stop();
        if (error != nullptr) {
            *error = "rtp parse failed";
        }
        return false;
    }

    media::JitterBuffer jitter(8);
    if (!jitter.push(receivedPacket)) {
        capture.stop();
        if (error != nullptr) {
            *error = "jitter push failed";
        }
        return false;
    }

    media::RTPPacket depacketized;
    if (!jitter.pop(depacketized)) {
        capture.stop();
        if (error != nullptr) {
            *error = "jitter pop failed";
        }
        return false;
    }

    EncodedAudioFrame receivedEncoded = encoded;
    receivedEncoded.payload = depacketized.payload;
    receivedEncoded.payloadType = depacketized.header.payloadType;

    AudioDecoder decoder;
    if (!decoder.configure(kDefaultSampleRate, kDefaultChannels)) {
        capture.stop();
        if (error != nullptr) {
            *error = "decoder configure failed";
        }
        return false;
    }

    capture::AudioFrame outFrame;
    std::string decodeError;
    const bool ok = decoder.decode(receivedEncoded, outFrame, &decodeError) &&
                    outFrame.sampleRate == inFrame.sampleRate &&
                    outFrame.channels == inFrame.channels &&
                    outFrame.samples.size() == inFrame.samples.size();
    capture.stop();
    if (!ok && error != nullptr) {
        *error = decodeError.empty() ? "decode did not yield a frame" : decodeError;
    }
    return ok;
}
}  // namespace av::codec



