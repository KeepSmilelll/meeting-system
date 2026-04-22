#pragma once

#include "RTPSender.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

struct H264AccessUnit {
    std::vector<uint8_t> payload;
    int64_t pts{0};
    uint8_t payloadType{0};
};

std::vector<std::vector<uint8_t>> packetizeH264AnnexB(const std::vector<uint8_t>& encodedFrame,
                                                      std::size_t maxPayloadBytes);

class H264AccessUnitAssembler {
public:
    bool consume(const RTPPacket& packet,
                 H264AccessUnit& outAccessUnit,
                 bool* packetLossDetected = nullptr);

private:
    static void appendStartCode(std::vector<uint8_t>& target);
    static void appendCompleteNalu(std::vector<uint8_t>& target, const uint8_t* data, std::size_t size);

    uint32_t m_timestamp{0};
    bool m_hasTimestamp{false};
    uint16_t m_expectedSequence{0};
    bool m_hasExpectedSequence{false};
    bool m_accessUnitCorrupted{false};
    std::vector<uint8_t> m_accessUnit;
    std::vector<uint8_t> m_fragmentedNalu;
};

}  // namespace media
