#include "VideoRecvTelemetryPipeline.h"

#include <QDebug>

namespace av::session {

void VideoRecvTelemetryPipeline::onPacketAccepted(
    std::atomic<uint64_t>& receivedPacketCount,
    bool& loggedFirstPacket,
    const media::RTPPacket& packet,
    const std::function<void(std::string)>& statusCallback) const {
    receivedPacketCount.fetch_add(1, std::memory_order_acq_rel);
    if (loggedFirstPacket) {
        return;
    }

    loggedFirstPacket = true;
    qInfo().noquote() << "[screen-session] first RTP recv seq=" << packet.header.sequenceNumber
                      << "ts=" << packet.header.timestamp
                      << "bytes=" << packet.payload.size();
    if (statusCallback) {
        statusCallback("Video RTP packet received");
    }
}

}  // namespace av::session
