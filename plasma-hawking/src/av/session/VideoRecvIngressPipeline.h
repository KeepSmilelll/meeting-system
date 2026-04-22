#pragma once

#include "VideoRecvPipeline.h"

#include "net/media/UdpPeerSocket.h"

namespace av::session {

struct VideoRecvIngressGate {
    bool acceptSender{false};
    bool acceptRtcpFromPeerHost{false};
};

enum class VideoRecvIngressAction {
    Ignore,
    Rtcp,
    Rtp,
};

class VideoRecvIngressPipeline {
public:
    VideoRecvIngressGate evaluateGate(const media::UdpPeerSocket& socket,
                                      const media::UdpEndpoint& from) const;

    VideoRecvIngressAction resolveEntryAction(VideoRecvDatagramKind datagramKind) const;
};

}  // namespace av::session
