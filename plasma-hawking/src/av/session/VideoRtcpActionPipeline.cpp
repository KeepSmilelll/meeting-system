#include "VideoRtcpActionPipeline.h"

#include <algorithm>
#include <array>

namespace av::session {

VideoRtcpActionPipeline::VideoRtcpActionPipeline(std::size_t retransmitCacheLimit)
    : m_retransmitCacheLimit((std::max)(std::size_t{1}, retransmitCacheLimit)) {}

void VideoRtcpActionPipeline::reset() {
    m_sentPacketCache.clear();
}

void VideoRtcpActionPipeline::cacheSentPacket(uint16_t sequenceNumber,
                                              std::vector<uint8_t> packetBytes) {
    if (packetBytes.empty()) {
        return;
    }

    m_sentPacketCache.emplace_back(sequenceNumber, std::move(packetBytes));
    while (m_sentPacketCache.size() > m_retransmitCacheLimit) {
        m_sentPacketCache.pop_front();
    }
}

bool VideoRtcpActionPipeline::retransmitPacket(uint16_t sequenceNumber,
                                               const media::UdpPeerSocket& socket,
                                               std::string* error) const {
    if (error != nullptr) {
        error->clear();
    }
    if (!socket.isOpen() || !socket.hasPeer()) {
        return false;
    }

    for (auto it = m_sentPacketCache.rbegin(); it != m_sentPacketCache.rend(); ++it) {
        if (it->first != sequenceNumber) {
            continue;
        }

        const std::vector<uint8_t>& packetBytes = it->second;
        const int sent = socket.sendToPeer(packetBytes.data(), packetBytes.size());
        if (sent == static_cast<int>(packetBytes.size())) {
            return true;
        }
        if (error != nullptr) {
            *error = "retransmit sendto failed";
        }
        return false;
    }
    return false;
}

std::vector<uint8_t> VideoRtcpActionPipeline::buildPictureLossIndication(uint32_t senderSsrc,
                                                                         uint32_t mediaSsrc) const {
    if (mediaSsrc == 0U) {
        return {};
    }
    std::array<uint8_t, 12> packet{};
    packet[0] = static_cast<uint8_t>((2U << 6) | 1U);  // v=2, fmt=PLI
    packet[1] = 206U;                                   // PSFB
    packet[2] = 0U;
    packet[3] = 2U;                                     // length words - 1

    packet[4] = static_cast<uint8_t>((senderSsrc >> 24) & 0xFFU);
    packet[5] = static_cast<uint8_t>((senderSsrc >> 16) & 0xFFU);
    packet[6] = static_cast<uint8_t>((senderSsrc >> 8) & 0xFFU);
    packet[7] = static_cast<uint8_t>(senderSsrc & 0xFFU);

    packet[8] = static_cast<uint8_t>((mediaSsrc >> 24) & 0xFFU);
    packet[9] = static_cast<uint8_t>((mediaSsrc >> 16) & 0xFFU);
    packet[10] = static_cast<uint8_t>((mediaSsrc >> 8) & 0xFFU);
    packet[11] = static_cast<uint8_t>(mediaSsrc & 0xFFU);

    return std::vector<uint8_t>(packet.begin(), packet.end());
}

bool VideoRtcpActionPipeline::sendPictureLossIndication(const media::UdpPeerSocket& socket,
                                                        uint32_t senderSsrc,
                                                        uint32_t mediaSsrc,
                                                        std::string* error) const {
    if (error != nullptr) {
        error->clear();
    }
    if (!socket.isOpen() || !socket.hasPeer() || mediaSsrc == 0U) {
        return false;
    }

    const std::vector<uint8_t> packet = buildPictureLossIndication(senderSsrc, mediaSsrc);
    if (packet.empty()) {
        return false;
    }
    const int sent = socket.sendToPeer(packet.data(), packet.size());
    if (sent == static_cast<int>(packet.size())) {
        return true;
    }
    if (error != nullptr) {
        *error = "PLI sendto failed";
    }
    return false;
}

}  // namespace av::session
