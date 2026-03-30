#include "AudioDecoder.h"

#include "av/capture/AudioCapture.h"
#include "av/codec/AudioEncoder.h"
#include "net/media/JitterBuffer.h"
#include "net/media/RTPReceiver.h"
#include "net/media/RTPSender.h"

#include <chrono>
#include <cstdint>

namespace av::codec {
namespace {

inline float pcm16ToFloat(int16_t v) {
    return static_cast<float>(v) / 32768.0F;
}

}  // namespace

AudioDecoder::AudioDecoder() {
    setDefaultChannelLayout(m_channelLayout, m_channels);
}

AudioDecoder::~AudioDecoder() {
    av_channel_layout_uninit(&m_channelLayout);
}

bool AudioDecoder::configure(int sampleRate, int channels) {
    if (sampleRate <= 0 || channels <= 0) {
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
    return true;
}

bool AudioDecoder::decode(const EncodedAudioFrame& inFrame, capture::AudioFrame& outFrame) const {
    if (inFrame.sampleRate != m_sampleRate || inFrame.channels != m_channels) {
        return false;
    }
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

    return true;
}

int AudioDecoder::sampleRate() const {
    return m_sampleRate;
}

int AudioDecoder::channels() const {
    return m_channels;
}

bool runAudioPipelineLoopbackSelfCheck() {
    capture::AudioCapture capture(8);
    if (!capture.start()) {
        return false;
    }

    capture::AudioFrame inFrame;
    inFrame.sampleRate = 48000;
    inFrame.channels = 1;
    inFrame.pts = 960;
    inFrame.samples.assign(960, 0.25F);

    if (!capture.pushCapturedFrame(inFrame)) {
        capture.stop();
        return false;
    }

    capture::AudioFrame captured;
    if (!capture.popFrameForEncode(captured, std::chrono::milliseconds(20))) {
        capture.stop();
        return false;
    }

    AudioEncoder encoder;
    if (!encoder.configure(48000, 1, 32000)) {
        capture.stop();
        return false;
    }

    EncodedAudioFrame encoded;
    if (!encoder.encode(captured, encoded)) {
        capture.stop();
        return false;
    }

    media::RTPSender sender(0x10203040, 10);
    const auto wire = sender.buildPacket(encoded.payloadType, true, static_cast<uint32_t>(captured.pts), encoded.payload);
    if (wire.empty()) {
        capture.stop();
        return false;
    }

    media::RTPReceiver receiver;
    media::RTPPacket receivedPacket;
    if (!receiver.parsePacket(wire, receivedPacket)) {
        capture.stop();
        return false;
    }

    media::JitterBuffer jitter(8);
    if (!jitter.push(receivedPacket)) {
        capture.stop();
        return false;
    }

    media::RTPPacket depacketized;
    if (!jitter.pop(depacketized)) {
        capture.stop();
        return false;
    }

    EncodedAudioFrame receivedEncoded = encoded;
    receivedEncoded.payload = depacketized.payload;

    AudioDecoder decoder;
    if (!decoder.configure(48000, 1)) {
        capture.stop();
        return false;
    }

    capture::AudioFrame outFrame;
    const bool ok = decoder.decode(receivedEncoded, outFrame) && outFrame.samples.size() == inFrame.samples.size();
    capture.stop();
    return ok;
}

}  // namespace av::codec


