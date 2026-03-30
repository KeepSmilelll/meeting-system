#include "RTCPHandler.h"

namespace media {
namespace {

inline uint16_t readBE16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

}  // namespace

bool RTCPHandler::parseHeader(const uint8_t* data, std::size_t len, RTCPHeader& outHeader) const {
    if (data == nullptr || len < 4) {
        return false;
    }

    const uint8_t b0 = data[0];
    outHeader.version = static_cast<uint8_t>((b0 >> 6) & 0x03);
    outHeader.padding = (b0 & 0x20) != 0;
    outHeader.countOrFormat = static_cast<uint8_t>(b0 & 0x1F);
    outHeader.packetType = data[1];
    outHeader.lengthWords = readBE16(data + 2);

    if (outHeader.version != 2) {
        return false;
    }

    const std::size_t packetBytes = packetSizeBytes(outHeader);
    if (packetBytes < 4 || packetBytes > len) {
        return false;
    }

    return true;
}

std::size_t RTCPHandler::packetSizeBytes(const RTCPHeader& header) const {
    return static_cast<std::size_t>(header.lengthWords + 1) * 4;
}

bool RTCPHandler::isReceiverReport(const RTCPHeader& header) const {
    return header.packetType == 201;  // RR
}

bool RTCPHandler::isSenderReport(const RTCPHeader& header) const {
    return header.packetType == 200;  // SR
}

bool RTCPHandler::isFeedbackPacket(const RTCPHeader& header) const {
    return header.packetType == 205 || header.packetType == 206;  // RTPFB / PSFB
}

}  // namespace media
