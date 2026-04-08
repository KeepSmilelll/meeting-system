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

struct RtcpReceptionReport {
    RtcpType packetType{RtcpType::Unknown};
    uint32_t reporterSsrc{0};
    uint32_t sourceSsrc{0};
    uint8_t fractionLost{0};
    int32_t cumulativeLost{0};
    uint32_t highestSeq{0};
    uint32_t jitter{0};
    uint32_t lastSenderReport{0};
    uint32_t delaySinceLastSenderReport{0};
};

struct RtcpSenderReport {
    uint32_t senderSsrc{0};
    uint64_t ntpTimestamp{0};
    uint32_t rtpTimestamp{0};
    uint32_t packetCount{0};
    uint32_t octetCount{0};
};

struct RtcpNackFeedback {
    uint32_t senderSsrc{0};
    uint32_t mediaSsrc{0};
    std::vector<uint16_t> lostSequences;
};

struct RtcpPliFeedback {
    uint32_t senderSsrc{0};
    uint32_t mediaSsrc{0};
};

struct RtcpRembFeedback {
    uint32_t senderSsrc{0};
    uint32_t mediaSsrc{0};
    uint8_t bitrateExp{0};
    uint32_t bitrateMantissa{0};
    uint32_t bitrateBps{0};
    std::vector<uint32_t> targetSsrcs;
};

class RtcpHandler final {
public:
    std::vector<RtcpPacketSummary> ParseCompound(const uint8_t* data, std::size_t len) const;
    std::vector<RtcpReceptionReport> ParseReceptionReports(const uint8_t* data, std::size_t len) const;
    std::vector<RtcpSenderReport> ParseSenderReports(const uint8_t* data, std::size_t len) const;
    std::vector<RtcpNackFeedback> ParseNackFeedback(const uint8_t* data, std::size_t len) const;
    std::vector<RtcpPliFeedback> ParsePliFeedback(const uint8_t* data, std::size_t len) const;
    std::vector<RtcpRembFeedback> ParseRembFeedback(const uint8_t* data, std::size_t len) const;

    static bool IsNack(const RtcpPacketSummary& pkt);
    static bool IsPli(const RtcpPacketSummary& pkt);
};

} // namespace sfu
