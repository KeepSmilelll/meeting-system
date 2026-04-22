#pragma once

#include "VideoRecvPipeline.h"

namespace av::session {

struct VideoRecvConsumeOutcome {
    bool ignored{true};
    uint32_t remoteMediaSsrc{0U};
    VideoRecvHandlingDecision decision;
};

class VideoRecvConsumePipeline {
public:
    bool consumeAndDecide(const media::RTPPacket& packet,
                          uint32_t expectedRemoteSsrc,
                          VideoRecvPipeline& recvPipeline,
                          av::codec::DecodedVideoFrame& outFrame,
                          VideoRecvConsumeOutcome& outOutcome) const;
    bool pollAndDecide(VideoRecvPipeline& recvPipeline,
                       av::codec::DecodedVideoFrame& outFrame,
                       VideoRecvConsumeOutcome& outOutcome) const;
};

}  // namespace av::session
