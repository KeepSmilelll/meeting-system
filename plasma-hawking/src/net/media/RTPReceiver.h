#pragma once

#include "RTPSender.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

class RTPReceiver {
public:
    bool parsePacket(const uint8_t* data, std::size_t len, RTPPacket& outPacket) const;
    bool parsePacket(const std::vector<uint8_t>& data, RTPPacket& outPacket) const;
};

// Minimal runtime self-check for RTP sender/receiver parse path.
bool runRtpLoopbackSelfCheck();

}  // namespace media
