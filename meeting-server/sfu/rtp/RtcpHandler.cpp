#include "rtp/RtcpHandler.h"

namespace sfu {
namespace {

inline uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

inline uint32_t ReadU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline RtcpType ToRtcpType(uint8_t pt) {
    switch (pt) {
    case 200:
        return RtcpType::SR;
    case 201:
        return RtcpType::RR;
    case 205:
        return RtcpType::RTPFB;
    case 206:
        return RtcpType::PSFB;
    default:
        return RtcpType::Unknown;
    }
}

} // namespace

std::vector<RtcpPacketSummary> RtcpHandler::ParseCompound(const uint8_t* data, std::size_t len) const {
    std::vector<RtcpPacketSummary> packets;
    if (data == nullptr || len < 4) {
        return packets;
    }

    std::size_t offset = 0;
    while (offset + 4 <= len) {
        const uint8_t vpc = data[offset];
        const uint8_t version = static_cast<uint8_t>((vpc >> 6) & 0x03);
        if (version != 2) {
            break;
        }

        const uint8_t fmt = static_cast<uint8_t>(vpc & 0x1F);
        const uint8_t pt = data[offset + 1];
        const uint16_t lengthWords = ReadU16(data + offset + 2);
        const std::size_t packetSize = static_cast<std::size_t>(lengthWords + 1) * 4;

        if (packetSize < 4 || offset + packetSize > len) {
            break;
        }

        RtcpPacketSummary summary;
        summary.type = ToRtcpType(pt);
        summary.fmt = fmt;
        summary.lengthWords = lengthWords;
        summary.offset = offset;
        summary.packetSize = packetSize;
        if (packetSize >= 8) {
            summary.senderSsrc = ReadU32(data + offset + 4);
        }

        packets.push_back(summary);
        offset += packetSize;
    }

    return packets;
}

bool RtcpHandler::IsNack(const RtcpPacketSummary& pkt) {
    // RFC 4585: RTPFB + FMT=1 -> Generic NACK
    return pkt.type == RtcpType::RTPFB && pkt.fmt == 1;
}

bool RtcpHandler::IsPli(const RtcpPacketSummary& pkt) {
    // RFC 4585: PSFB + FMT=1 -> PLI
    return pkt.type == RtcpType::PSFB && pkt.fmt == 1;
}

} // namespace sfu
