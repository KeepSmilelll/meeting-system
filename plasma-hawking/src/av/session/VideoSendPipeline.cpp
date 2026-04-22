#include "VideoSendPipeline.h"

#include "net/media/H264RtpPayload.h"
#include "net/media/SocketAddressUtils.h"

#include <algorithm>

namespace av::session {
namespace {

bool packetizeEncodedFrame(const av::codec::EncodedVideoFrame& encoded,
                           int fallbackFrameRate,
                           media::RTPSender& sender,
                           std::size_t maxPayloadBytes,
                           std::vector<VideoSendPipelinePacket>& outPackets,
                           std::string* error) {
    const auto payloads = media::packetizeH264AnnexB(encoded.payload, maxPayloadBytes);
    if (payloads.empty()) {
        if (error != nullptr) {
            *error = "no RTP payloads produced from H264 frame";
        }
        return false;
    }

    const int frameRate = encoded.frameRate > 0 ? encoded.frameRate : fallbackFrameRate;
    const uint32_t timestamp = static_cast<uint32_t>((encoded.pts * 90000LL) /
                                                     (std::max)(1, frameRate));

    outPackets.reserve(payloads.size());
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        const bool marker = (i + 1 == payloads.size());
        std::vector<uint8_t> packet = sender.buildPacket(encoded.payloadType, marker, timestamp, payloads[i]);
        if (packet.empty()) {
            if (error != nullptr) {
                *error = "RTP packet build failed";
            }
            outPackets.clear();
            return false;
        }

        VideoSendPipelinePacket outPacket;
        outPacket.timestamp = timestamp;
        outPacket.sequenceNumber = media::parseRtpSequenceNumber(packet.data(), packet.size());
        outPacket.bytes = std::move(packet);
        outPackets.push_back(std::move(outPacket));
    }

    return true;
}

void resetEncodeOutput(std::vector<VideoSendPipelinePacket>& outPackets,
                       bool* encodedKeyFrame,
                       std::string* error) {
    outPackets.clear();
    if (encodedKeyFrame != nullptr) {
        *encodedKeyFrame = false;
    }
    if (error != nullptr) {
        error->clear();
    }
}

}  // namespace

VideoSendPipeline::VideoSendPipeline(VideoSendPipelineConfig config)
    : m_config(config) {}

bool VideoSendPipeline::encodeAndPacketize(av::codec::VideoEncoder& encoder,
                                           const VideoSendPipelineInputFrame& inputFrame,
                                           uint8_t payloadType,
                                           bool forceKeyFrame,
                                           media::RTPSender& sender,
                                           std::vector<VideoSendPipelinePacket>& outPackets,
                                           bool* encodedKeyFrame,
                                           std::string* error) const {
    if (inputFrame.hasAvFrame()) {
        return encodeAndPacketize(encoder,
                                  *inputFrame.avFrame,
                                  payloadType,
                                  forceKeyFrame,
                                  sender,
                                  outPackets,
                                  encodedKeyFrame,
                                  error);
    }
    if (inputFrame.hasScreenFrame()) {
        return encodeAndPacketize(encoder,
                                  inputFrame.screenFrame,
                                  payloadType,
                                  forceKeyFrame,
                                  sender,
                                  outPackets,
                                  encodedKeyFrame,
                                  error);
    }

    resetEncodeOutput(outPackets, encodedKeyFrame, error);
    if (error != nullptr) {
        *error = "empty video send input frame";
    }
    return false;
}

bool VideoSendPipeline::encodeAndPacketize(av::codec::VideoEncoder& encoder,
                                           const av::capture::ScreenFrame& frame,
                                           uint8_t payloadType,
                                           bool forceKeyFrame,
                                           media::RTPSender& sender,
                                           std::vector<VideoSendPipelinePacket>& outPackets,
                                           bool* encodedKeyFrame,
                                           std::string* error) const {
    resetEncodeOutput(outPackets, encodedKeyFrame, error);

    encoder.setPayloadType(payloadType);
    if (forceKeyFrame) {
        encoder.requestKeyframe();
    }

    av::codec::EncodedVideoFrame encoded;
    std::string encodeError;
    if (!encoder.encode(frame, encoded, false, &encodeError)) {
        if (error != nullptr) {
            *error = encodeError;
        }
        return false;
    }
    if (encodedKeyFrame != nullptr) {
        *encodedKeyFrame = encoded.keyFrame;
    }

    return packetizeEncodedFrame(encoded, m_config.frameRate, sender, m_config.maxPayloadBytes, outPackets, error);
}

bool VideoSendPipeline::encodeAndPacketize(av::codec::VideoEncoder& encoder,
                                           const AVFrame& frame,
                                           uint8_t payloadType,
                                           bool forceKeyFrame,
                                           media::RTPSender& sender,
                                           std::vector<VideoSendPipelinePacket>& outPackets,
                                           bool* encodedKeyFrame,
                                           std::string* error) const {
    resetEncodeOutput(outPackets, encodedKeyFrame, error);

    encoder.setPayloadType(payloadType);
    if (forceKeyFrame) {
        encoder.requestKeyframe();
    }

    av::codec::EncodedVideoFrame encoded;
    std::string encodeError;
    if (!encoder.encode(frame, encoded, false, &encodeError)) {
        if (error != nullptr) {
            *error = encodeError;
        }
        return false;
    }
    if (encodedKeyFrame != nullptr) {
        *encodedKeyFrame = encoded.keyFrame;
    }

    return packetizeEncodedFrame(encoded, m_config.frameRate, sender, m_config.maxPayloadBytes, outPackets, error);
}

}  // namespace av::session
