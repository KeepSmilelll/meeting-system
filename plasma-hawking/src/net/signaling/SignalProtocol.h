#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace signaling {

constexpr uint8_t kVersion = 0x01;
constexpr uint8_t kMagicHigh = 0xAB;
constexpr uint8_t kMagicLow = 0xCD;
constexpr size_t kHeaderSize = 9;

struct FrameHeader {
    uint16_t type;
    uint32_t length;
};

std::vector<uint8_t> encodeFrame(uint16_t type, const std::vector<uint8_t>& payload);
std::optional<FrameHeader> decodeHeader(const std::vector<uint8_t>& bytes, uint32_t maxPayloadLen);

}  // namespace signaling
