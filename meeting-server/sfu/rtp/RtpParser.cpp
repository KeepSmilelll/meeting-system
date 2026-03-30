#include "rtp/RtpParser.h"

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

} // namespace

bool RtpParser::Parse(const uint8_t* data, std::size_t len, ParsedRtpPacket* out) const {
    if (data == nullptr || out == nullptr || len < kMinHeaderSize) {
        return false;
    }

    ParsedRtpPacket parsed{};
    const uint8_t b0 = data[0];
    const uint8_t b1 = data[1];

    parsed.header.version = static_cast<uint8_t>((b0 >> 6) & 0x03);
    if (parsed.header.version != 2) {
        return false;
    }

    parsed.header.padding = (b0 & 0x20U) != 0;
    parsed.header.extension = (b0 & 0x10U) != 0;
    parsed.header.csrcCount = static_cast<uint8_t>(b0 & 0x0FU);

    parsed.header.marker = (b1 & 0x80U) != 0;
    parsed.header.payloadType = static_cast<uint8_t>(b1 & 0x7FU);
    parsed.header.sequence = ReadU16(data + 2);
    parsed.header.timestamp = ReadU32(data + 4);
    parsed.header.ssrc = ReadU32(data + 8);

    std::size_t offset = kMinHeaderSize;
    const std::size_t csrcBytes = static_cast<std::size_t>(parsed.header.csrcCount) * 4;
    if (offset + csrcBytes > len) {
        return false;
    }
    offset += csrcBytes;

    if (parsed.header.extension) {
        if (offset + 4 > len) {
            return false;
        }

        const uint16_t extWords = ReadU16(data + offset + 2);
        const std::size_t extBytes = static_cast<std::size_t>(extWords) * 4;
        if (offset + 4 + extBytes > len) {
            return false;
        }
        offset += 4 + extBytes;
    }

    std::size_t payloadSize = len - offset;
    if (parsed.header.padding) {
        if (payloadSize == 0) {
            return false;
        }

        const uint8_t padBytes = data[len - 1];
        if (padBytes == 0 || padBytes > payloadSize) {
            return false;
        }
        payloadSize -= padBytes;
    }

    parsed.header.headerSize = offset;
    parsed.payload = data + offset;
    parsed.payloadSize = payloadSize;

    *out = parsed;
    return true;
}

} // namespace sfu
