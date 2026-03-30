#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfu {

enum class RtcpType : uint8_t {
    SR = 200,
    RR = 201,
    RTPFB = 205,
    PSFB = 206,
    Unknown = 0
};

struct RtcpPacketSummary {
    RtcpType type{RtcpType::Unknown};
    uint8_t fmt{0};
    uint16_t lengthWords{0};
    uint32_t senderSsrc{0};
    std::size_t offset{0};
    std::size_t packetSize{0};
};

class RtcpHandler final {
public:
    std::vector<RtcpPacketSummary> ParseCompound(const uint8_t* data, std::size_t len) const;

    static bool IsNack(const RtcpPacketSummary& pkt);
    static bool IsPli(const RtcpPacketSummary& pkt);
};

} // namespace sfu
