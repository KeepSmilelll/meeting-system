#include "VideoRecvIngressPipeline.h"

namespace av::session {

VideoRecvIngressGate VideoRecvIngressPipeline::evaluateGate(const media::UdpPeerSocket& socket,
                                                            const media::UdpEndpoint& from) const {
    VideoRecvIngressGate gate{};
    gate.acceptSender = socket.acceptSender(from);
    gate.acceptRtcpFromPeerHost = socket.acceptRtcpFromPeerHost(from);
    return gate;
}

VideoRecvIngressAction VideoRecvIngressPipeline::resolveEntryAction(
    VideoRecvDatagramKind datagramKind) const {
    switch (datagramKind) {
    case VideoRecvDatagramKind::Ignore:
        return VideoRecvIngressAction::Ignore;
    case VideoRecvDatagramKind::Rtcp:
        return VideoRecvIngressAction::Rtcp;
    case VideoRecvDatagramKind::Rtp:
        return VideoRecvIngressAction::Rtp;
    default:
        return VideoRecvIngressAction::Ignore;
    }
}

}  // namespace av::session
