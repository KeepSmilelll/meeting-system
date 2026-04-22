#include "VideoRecvConsumePipeline.h"

namespace av::session {

bool VideoRecvConsumePipeline::consumeAndDecide(const media::RTPPacket& packet,
                                                uint32_t expectedRemoteSsrc,
                                                VideoRecvPipeline& recvPipeline,
                                                av::codec::DecodedVideoFrame& outFrame,
                                                VideoRecvConsumeOutcome& outOutcome) const {
    outOutcome = VideoRecvConsumeOutcome{};

    std::string pipelineError;
    const VideoRecvPacketResult result = recvPipeline.consumePacket(
        packet,
        expectedRemoteSsrc,
        outFrame,
        outOutcome.remoteMediaSsrc,
        &pipelineError);
    if (result == VideoRecvPacketResult::Ignored) {
        return false;
    }

    outOutcome.ignored = false;
    outOutcome.decision = recvPipeline.makeHandlingDecision(result, pipelineError);
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
