#include "rtp/RtcpHandler.h"

#include <cstring>
#include <limits>
#include <utility>

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

inline uint32_t ReadU24(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

inline int32_t ReadS24(const uint8_t* p) {
    uint32_t value = ReadU24(p);
    if ((value & 0x00800000U) != 0U) {
        value |= 0xFF000000U;
    }
    return static_cast<int32_t>(value);
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

std::vector<RtcpReceptionReport> RtcpHandler::ParseReceptionReports(const uint8_t* data, std::size_t len) const {
    std::vector<RtcpReceptionReport> reports;
    const auto packets = ParseCompound(data, len);
    for (const auto& packet : packets) {
        if (packet.type != RtcpType::RR && packet.type != RtcpType::SR) {
            continue;
        }

        const std::size_t reportCount = packet.fmt;
        const std::size_t baseOffset = packet.offset + (packet.type == RtcpType::SR ? 28U : 8U);
        if (packet.packetSize < baseOffset - packet.offset) {
            continue;
        }

        for (std::size_t i = 0; i < reportCount; ++i) {
            const std::size_t blockOffset = baseOffset + i * 24U;
            if (blockOffset + 24U > packet.offset + packet.packetSize || blockOffset + 24U > len) {
                break;
            }

            RtcpReceptionReport report;
            report.packetType = packet.type;
            report.reporterSsrc = packet.senderSsrc;
            report.sourceSsrc = ReadU32(data + blockOffset);
            report.fractionLost = data[blockOffset + 4];
            report.cumulativeLost = ReadS24(data + blockOffset + 5);
            report.highestSeq = ReadU32(data + blockOffset + 8);
            report.jitter = ReadU32(data + blockOffset + 12);
            report.lastSenderReport = ReadU32(data + blockOffset + 16);
            report.delaySinceLastSenderReport = ReadU32(data + blockOffset + 20);
            reports.push_back(report);
        }
    }
    return reports;
}

std::vector<RtcpSenderReport> RtcpHandler::ParseSenderReports(const uint8_t* data, std::size_t len) const {
    std::vector<RtcpSenderReport> reports;
    const auto packets = ParseCompound(data, len);
    for (const auto& packet : packets) {
        if (packet.type != RtcpType::SR || packet.packetSize < 28U || packet.offset + 28U > len) {
            continue;
        }

        const uint8_t* body = data + packet.offset + 4U;
        RtcpSenderReport report;
        report.senderSsrc = ReadU32(body);
        report.ntpTimestamp = (static_cast<uint64_t>(ReadU32(body + 4U)) << 32U) | ReadU32(body + 8U);
        report.rtpTimestamp = ReadU32(body + 12U);
        report.packetCount = ReadU32(body + 16U);
        report.octetCount = ReadU32(body + 20U);
        reports.push_back(report);
    }
    return reports;
}

std::vector<RtcpNackFeedback> RtcpHandler::ParseNackFeedback(const uint8_t* data, std::size_t len) const {
    std::vector<RtcpNackFeedback> nacks;
    const auto packets = ParseCompound(data, len);
    for (const auto& packet : packets) {
        if (!IsNack(packet) || packet.packetSize < 12U || packet.offset + packet.packetSize > len) {
            continue;
        }

        const uint8_t* body = data + packet.offset + 4U;
        RtcpNackFeedback feedback;
        feedback.senderSsrc = ReadU32(body);
        feedback.mediaSsrc = ReadU32(body + 4U);

        const std::size_t fciOffset = packet.offset + 12U;
        const std::size_t fciEnd = packet.offset + packet.packetSize;
        for (std::size_t blockOffset = fciOffset; blockOffset + 4U <= fciEnd; blockOffset += 4U) {
            const uint16_t pid = ReadU16(data + blockOffset);
            const uint16_t blp = ReadU16(data + blockOffset + 2U);

            feedback.lostSequences.push_back(pid);
            for (uint16_t bit = 0; bit < 16U; ++bit) {
                if ((blp & static_cast<uint16_t>(1U << bit)) == 0U) {
                    continue;
                }
                feedback.lostSequences.push_back(static_cast<uint16_t>(pid + bit + 1U));
            }
        }

        if (!feedback.lostSequences.empty()) {
            nacks.push_back(std::move(feedback));
        }
    }

    return nacks;
}

std::vector<RtcpPliFeedback> RtcpHandler::ParsePliFeedback(const uint8_t* data, std::size_t len) const {
    std::vector<RtcpPliFeedback> plis;
    const auto packets = ParseCompound(data, len);
    for (const auto& packet : packets) {
        if (!IsPli(packet) || packet.packetSize < 12U || packet.offset + packet.packetSize > len) {
            continue;
        }

        const uint8_t* body = data + packet.offset + 4U;
        RtcpPliFeedback feedback;
        feedback.senderSsrc = ReadU32(body);
        feedback.mediaSsrc = ReadU32(body + 4U);
        plis.push_back(feedback);
    }

    return plis;
}

std::vector<RtcpRembFeedback> RtcpHandler::ParseRembFeedback(const uint8_t* data, std::size_t len) const {
    std::vector<RtcpRembFeedback> rembs;
    const auto packets = ParseCompound(data, len);
    for (const auto& packet : packets) {
        if (packet.type != RtcpType::PSFB || packet.fmt != 15 || packet.packetSize < 20U || packet.offset + packet.packetSize > len) {
            continue;
        }

        const uint8_t* body = data + packet.offset + 4U;
        const uint8_t* fci = body + 8U;
        if (std::memcmp(fci, "REMB", 4) != 0) {
            continue;
        }

        const uint8_t ssrcCount = fci[4];
        const std::size_t requiredBytes = 20U + static_cast<std::size_t>(ssrcCount) * 4U;
        if (packet.packetSize < requiredBytes) {
            continue;
        }

        RtcpRembFeedback feedback;
        feedback.senderSsrc = ReadU32(body);
        feedback.mediaSsrc = ReadU32(body + 4U);
        feedback.bitrateExp = static_cast<uint8_t>((fci[5] >> 2U) & 0x3FU);
        feedback.bitrateMantissa = (static_cast<uint32_t>(fci[5] & 0x03U) << 16U) |
                                   (static_cast<uint32_t>(fci[6]) << 8U) |
                                   static_cast<uint32_t>(fci[7]);

        const uint64_t bitrate = static_cast<uint64_t>(feedback.bitrateMantissa) << feedback.bitrateExp;
        feedback.bitrateBps = bitrate > std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(bitrate);

        for (uint8_t i = 0; i < ssrcCount; ++i) {
            feedback.targetSsrcs.push_back(ReadU32(fci + 8U + static_cast<std::size_t>(i) * 4U));
        }
        if (!feedback.targetSsrcs.empty()) {
            rembs.push_back(std::move(feedback));
        }
    }

    return rembs;
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
