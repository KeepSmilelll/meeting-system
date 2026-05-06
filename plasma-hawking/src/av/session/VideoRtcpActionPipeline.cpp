#include "VideoRtcpActionPipeline.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace av::session {
namespace {

void appendUint16(std::vector<uint8_t>& packet, uint16_t value) {
    packet.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    packet.push_back(static_cast<uint8_t>(value & 0xFFU));
}

void appendUint32(std::vector<uint8_t>& packet, uint32_t value) {
    packet.push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
    packet.push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
    packet.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    packet.push_back(static_cast<uint8_t>(value & 0xFFU));
}

}  // namespace

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

std::vector<uint8_t> VideoRtcpActionPipeline::buildNackFeedback(
    uint32_t senderSsrc,
    uint32_t mediaSsrc,
    const std::vector<uint16_t>& lostSequences) const {
    if (mediaSsrc == 0U || lostSequences.empty()) {
        return {};
    }

    std::vector<uint16_t> sequences = lostSequences;
    std::sort(sequences.begin(), sequences.end());
    sequences.erase(std::unique(sequences.begin(), sequences.end()), sequences.end());
    if (sequences.empty()) {
        return {};
    }

    struct NackBlock {
        uint16_t pid{0};
        uint16_t blp{0};
    };
    std::vector<NackBlock> blocks;
    for (std::size_t i = 0; i < sequences.size();) {
        NackBlock block{};
        block.pid = sequences[i];
        ++i;
        while (i < sequences.size()) {
            const uint16_t delta = static_cast<uint16_t>(sequences[i] - block.pid);
            if (delta == 0U) {
                ++i;
                continue;
            }
            if (delta > 16U) {
                break;
            }
            block.blp |= static_cast<uint16_t>(1U << (delta - 1U));
            ++i;
        }
        blocks.push_back(block);
    }

    const std::size_t packetBytes = 12U + blocks.size() * 4U;
    if (packetBytes > 0xFFFFU * 4U) {
        return {};
    }
    const uint16_t lengthWordsMinusOne =
        static_cast<uint16_t>((packetBytes / 4U) - 1U);

    std::vector<uint8_t> packet;
    packet.reserve(packetBytes);
    packet.push_back(static_cast<uint8_t>((2U << 6) | 1U));  // v=2, fmt=Generic NACK
    packet.push_back(205U);                                  // RTPFB
    appendUint16(packet, lengthWordsMinusOne);
    appendUint32(packet, senderSsrc);
    appendUint32(packet, mediaSsrc);
    for (const NackBlock& block : blocks) {
        appendUint16(packet, block.pid);
        appendUint16(packet, block.blp);
    }
    return packet;
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

bool VideoRtcpActionPipeline::sendNackFeedback(const media::UdpPeerSocket& socket,
                                               uint32_t senderSsrc,
                                               uint32_t mediaSsrc,
                                               const std::vector<uint16_t>& lostSequences,
                                               std::string* error) const {
    if (error != nullptr) {
        error->clear();
    }
    if (!socket.isOpen() || !socket.hasPeer() || mediaSsrc == 0U) {
        return false;
    }

    const std::vector<uint8_t> packet = buildNackFeedback(senderSsrc, mediaSsrc, lostSequences);
    if (packet.empty()) {
        return false;
    }
    const int sent = socket.sendToPeer(packet.data(), packet.size());
    if (sent == static_cast<int>(packet.size())) {
        return true;
    }
    if (error != nullptr) {
        *error = "NACK sendto failed";
    }
    return false;
}

}  // namespace av::session
