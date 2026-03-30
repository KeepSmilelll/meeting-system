#pragma once

#include <cstddef>
#include <cstdint>

namespace media {

struct RTCPHeader {
    uint8_t version{0};
    bool padding{false};
    uint8_t countOrFormat{0};
    uint8_t packetType{0};
    uint16_t lengthWords{0};
};

class RTCPHandler {
public:
    bool parseHeader(const uint8_t* data, std::size_t len, RTCPHeader& outHeader) const;
    std::size_t packetSizeBytes(const RTCPHeader& header) const;

    bool isReceiverReport(const RTCPHeader& header) const;
    bool isSenderReport(const RTCPHeader& header) const;
    bool isFeedbackPacket(const RTCPHeader& header) const;
};

}  // namespace media
