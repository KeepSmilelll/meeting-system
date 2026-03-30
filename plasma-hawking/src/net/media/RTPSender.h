#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

constexpr std::size_t kRtpMinHeaderSize = 12;

struct RTPHeader {
    uint8_t version{2};
    bool padding{false};
    bool extension{false};
    uint8_t csrcCount{0};
    bool marker{false};
    uint8_t payloadType{0};
    uint16_t sequenceNumber{0};
    uint32_t timestamp{0};
    uint32_t ssrc{0};
};

struct RTPPacket {
    RTPHeader header;
    std::vector<uint8_t> payload;
};

bool parseRTPHeader(const uint8_t* data, std::size_t len, RTPHeader& outHeader, std::size_t& payloadOffset);

class RTPSender {
public:
    explicit RTPSender(uint32_t ssrc = 0, uint16_t initialSequence = 0);

    void setSSRC(uint32_t ssrc);
    uint32_t ssrc() const;

    void setSequence(uint16_t sequence);
    uint16_t sequence() const;

    std::vector<uint8_t> buildPacket(uint8_t payloadType,
                                     bool marker,
                                     uint32_t timestamp,
                                     const uint8_t* payload,
                                     std::size_t payloadLen);

    std::vector<uint8_t> buildPacket(uint8_t payloadType,
                                     bool marker,
                                     uint32_t timestamp,
                                     const std::vector<uint8_t>& payload);

private:
    std::atomic<uint32_t> m_ssrc;
    std::atomic<uint16_t> m_sequence;
};

}  // namespace media
