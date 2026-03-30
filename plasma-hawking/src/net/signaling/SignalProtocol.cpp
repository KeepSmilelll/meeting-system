#include "SignalProtocol.h"

#include <algorithm>

namespace signaling {

std::vector<uint8_t> encodeFrame(uint16_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame(kHeaderSize + payload.size());

    frame[0] = kMagicHigh;
    frame[1] = kMagicLow;
    frame[2] = kVersion;

    frame[3] = static_cast<uint8_t>((type >> 8) & 0xFF);
    frame[4] = static_cast<uint8_t>(type & 0xFF);

    const auto payloadLen = static_cast<uint32_t>(payload.size());
    frame[5] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
    frame[6] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
    frame[7] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    frame[8] = static_cast<uint8_t>(payloadLen & 0xFF);

    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), frame.begin() + static_cast<long long>(kHeaderSize));
    }

    return frame;
}

std::optional<FrameHeader> decodeHeader(const std::vector<uint8_t>& bytes, uint32_t maxPayloadLen) {
    if (bytes.size() != kHeaderSize) {
        return std::nullopt;
    }

    if (bytes[0] != kMagicHigh || bytes[1] != kMagicLow || bytes[2] != kVersion) {
        return std::nullopt;
    }

    const uint16_t type = static_cast<uint16_t>((static_cast<uint16_t>(bytes[3]) << 8) | bytes[4]);
    const uint32_t length = (static_cast<uint32_t>(bytes[5]) << 24) |
                            (static_cast<uint32_t>(bytes[6]) << 16) |
                            (static_cast<uint32_t>(bytes[7]) << 8) |
                            static_cast<uint32_t>(bytes[8]);

    if (length > maxPayloadLen) {
        return std::nullopt;
    }

    return FrameHeader{type, length};
}

}  // namespace signaling
