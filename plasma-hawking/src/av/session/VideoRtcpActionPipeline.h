#pragma once

#include "net/media/UdpPeerSocket.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace av::session {

class VideoRtcpActionPipeline {
public:
    explicit VideoRtcpActionPipeline(std::size_t retransmitCacheLimit = 512U);

    void reset();
    void cacheSentPacket(uint16_t sequenceNumber, std::vector<uint8_t> packetBytes);

    bool retransmitPacket(uint16_t sequenceNumber,
                          const media::UdpPeerSocket& socket,
                          std::string* error = nullptr) const;

    std::vector<uint8_t> buildPictureLossIndication(uint32_t senderSsrc,
                                                    uint32_t mediaSsrc) const;

    bool sendPictureLossIndication(const media::UdpPeerSocket& socket,
                                   uint32_t senderSsrc,
                                   uint32_t mediaSsrc,
                                   std::string* error = nullptr) const;

private:
    std::size_t m_retransmitCacheLimit{512U};
    std::deque<std::pair<uint16_t, std::vector<uint8_t>>> m_sentPacketCache;
};

}  // namespace av::session
