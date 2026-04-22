#pragma once

#include "av/codec/VideoDecoder.h"
#include "net/media/H264RtpPayload.h"
#include "net/media/RTPReceiver.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace av::session {

class VideoRecvDecodeWorker;

struct VideoRecvPipelineConfig {
    uint8_t screenPayloadType{97};
    uint8_t cameraPayloadType{96};
    int frameRate{5};
};

enum class VideoRecvPacketResult {
    Ignored,
    NeedMore,
    PacketLoss,
    DecodePending,
    DecodeFailed,
    FrameReady,
};

enum class VideoRecvDatagramKind {
    Ignore,
    Rtcp,
    Rtp,
};

struct VideoRecvDatagram {
    VideoRecvDatagramKind kind{VideoRecvDatagramKind::Ignore};
    media::RTPPacket rtpPacket;
};

enum class VideoRecvHandlingAction {
    Continue,
    RequestKeyFrame,
    RequestKeyFrameAndError,
    DeliverFrame,
};

struct VideoRecvHandlingDecision {
    VideoRecvHandlingAction action{VideoRecvHandlingAction::Continue};
    const char* keyFrameReason{nullptr};
    std::string errorMessage;
};

class VideoRecvPipeline {
public:
    explicit VideoRecvPipeline(VideoRecvPipelineConfig config = {});
    ~VideoRecvPipeline();

    VideoRecvDatagram classifyDatagram(const uint8_t* data,
                                       std::size_t len,
                                       bool acceptSender,
                                       bool acceptRtcpFromPeerHost,
                                       media::RTPReceiver& receiver) const;

    VideoRecvPacketResult consumePacket(const media::RTPPacket& packet,
                                        uint32_t expectedRemoteSsrc,
                                        av::codec::DecodedVideoFrame& outFrame,
                                        uint32_t& outRemoteMediaSsrc,
                                        std::string* error = nullptr);
    VideoRecvPacketResult pollDecodedFrame(av::codec::DecodedVideoFrame& outFrame,
                                           uint32_t& outRemoteMediaSsrc,
                                           std::string* error = nullptr);

    VideoRecvHandlingDecision makeHandlingDecision(VideoRecvPacketResult result,
                                                   const std::string& pipelineError) const;

    std::size_t decodeWorkerCount() const;
    std::size_t distinctDecodeThreadCount() const;

private:
    struct StreamState {
        media::H264AccessUnitAssembler assembler;
        std::shared_ptr<VideoRecvDecodeWorker> decodeWorker;
        uint64_t lastSeenAtMs{0};
    };

    StreamState* findOrCreateStreamState(uint32_t ssrc,
                                         uint64_t nowMs,
                                         uint32_t expectedRemoteSsrc,
                                         std::string* error);
    VideoRecvPacketResult popDecodedFrameResult(uint32_t preferredSsrc,
                                                av::codec::DecodedVideoFrame& outFrame,
                                                uint32_t& outRemoteMediaSsrc,
                                                std::string* error);
    void pruneStreamStates(uint64_t nowMs, uint32_t expectedRemoteSsrc);

    VideoRecvPipelineConfig m_config;
    std::unordered_map<uint32_t, StreamState> m_streamStates;
};

}  // namespace av::session
