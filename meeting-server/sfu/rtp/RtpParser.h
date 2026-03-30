#pragma once

#include <cstddef>
#include <cstdint>

namespace sfu {

struct RtpHeader {
    uint8_t version{0};
    bool padding{false};
    bool extension{false};
    uint8_t csrcCount{0};
    bool marker{false};
    uint8_t payloadType{0};
    uint16_t sequence{0};
    uint32_t timestamp{0};
    uint32_t ssrc{0};
    std::size_t headerSize{0};
};

struct ParsedRtpPacket {
    RtpHeader header;
    const uint8_t* payload{nullptr};
    std::size_t payloadSize{0};
};

class RtpParser final {
public:
    static constexpr std::size_t kMinHeaderSize = 12;

    bool Parse(const uint8_t* data, std::size_t len, ParsedRtpPacket* out) const;
};

} // namespace sfu
