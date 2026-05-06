#include "VideoRecvConsumePipeline.h"

#include <utility>

namespace av::session {

bool VideoRecvConsumePipeline::consumeAndDecide(const media::RTPPacket& packet,
                                                uint32_t expectedRemoteSsrc,
                                                VideoRecvPipeline& recvPipeline,
                                                av::codec::DecodedVideoFrame& outFrame,
                                                VideoRecvConsumeOutcome& outOutcome) const {
    outOutcome = VideoRecvConsumeOutcome{};

    std::string pipelineError;
    media::H264PacketLossInfo lossInfo;
    const VideoRecvPacketResult result = recvPipeline.consumePacket(
        packet,
        expectedRemoteSsrc,
        outFrame,
        outOutcome.remoteMediaSsrc,
        &pipelineError,
        &lossInfo);
    if (result == VideoRecvPacketResult::Ignored) {
        return false;
    }

    outOutcome.ignored = false;
    outOutcome.missingSequences = std::move(lossInfo.missingSequences);
    outOutcome.decision = recvPipeline.makeHandlingDecision(result, pipelineError);
    if (result == VideoRecvPacketResult::PacketLoss && !outOutcome.missingSequences.empty()) {
        outOutcome.decision.action = VideoRecvHandlingAction::RequestRetransmit;
        outOutcome.decision.keyFrameReason = "packet loss";
        outOutcome.decision.retransmitSequenceNumbers = outOutcome.missingSequences;
    }
    return true;
}

bool VideoRecvConsumePipeline::pollAndDecide(VideoRecvPipeline& recvPipeline,
                                             av::codec::DecodedVideoFrame& outFrame,
                                             VideoRecvConsumeOutcome& outOutcome) const {
    outOutcome = VideoRecvConsumeOutcome{};

    std::string pipelineError;
    const VideoRecvPacketResult result = recvPipeline.pollDecodedFrame(
        outFrame,
        outOutcome.remoteMediaSsrc,
        &pipelineError);
    if (result == VideoRecvPacketResult::Ignored ||
        result == VideoRecvPacketResult::NeedMore ||
        result == VideoRecvPacketResult::DecodePending) {
        return false;
    }

    outOutcome.ignored = false;
    outOutcome.decision = recvPipeline.makeHandlingDecision(result, pipelineError);
    return true;
}

}  // namespace av::session
