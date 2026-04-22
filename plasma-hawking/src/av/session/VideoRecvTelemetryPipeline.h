#pragma once

#include "net/media/RTPReceiver.h"

#include <atomic>
#include <functional>
#include <string>

namespace av::session {

class VideoRecvTelemetryPipeline {
public:
    void onPacketAccepted(std::atomic<uint64_t>& receivedPacketCount,
                          bool& loggedFirstPacket,
                          const media::RTPPacket& packet,
                          const std::function<void(std::string)>& statusCallback) const;
};

}  // namespace av::session
