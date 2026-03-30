#include "RTPSender.h"

#include <algorithm>

namespace media {
namespace {

inline uint16_t readBE16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline void writeBE16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

inline void writeBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

}  // namespace

bool parseRTPHeader(const uint8_t* data, std::size_t len, RTPHeader& outHeader, std::size_t& payloadOffset) {
    if (data == nullptr || len < kRtpMinHeaderSize) {
        return false;
    }

    const uint8_t b0 = data[0];
    const uint8_t b1 = data[1];

    outHeader.version = static_cast<uint8_t>((b0 >> 6) & 0x03);
    outHeader.padding = (b0 & 0x20) != 0;
    outHeader.extension = (b0 & 0x10) != 0;
    outHeader.csrcCount = static_cast<uint8_t>(b0 & 0x0F);

    outHeader.marker = (b1 & 0x80) != 0;
    outHeader.payloadType = static_cast<uint8_t>(b1 & 0x7F);
    outHeader.sequenceNumber = readBE16(data + 2);
    outHeader.timestamp = readBE32(data + 4);
    outHeader.ssrc = readBE32(data + 8);

    if (outHeader.version != 2) {
        return false;
    }

    payloadOffset = kRtpMinHeaderSize + static_cast<std::size_t>(outHeader.csrcCount) * sizeof(uint32_t);
    if (payloadOffset > len) {
        return false;
    }

    if (outHeader.extension) {
        if (len < payloadOffset + 4) {
            return false;
        }

        const uint16_t extLengthWords = readBE16(data + payloadOffset + 2);
        const std::size_t extBytes = static_cast<std::size_t>(extLengthWords) * 4;
        payloadOffset += 4 + extBytes;

        if (payloadOffset > len) {
            return false;
        }
    }

    if (outHeader.padding) {
        const uint8_t padLen = data[len - 1];
        if (padLen == 0 || padLen > len - payloadOffset) {
            return false;
        }
    }

    return true;
}

RTPSender::RTPSender(uint32_t ssrc, uint16_t initialSequence)
    : m_ssrc(ssrc),
      m_sequence(initialSequence) {}

void RTPSender::setSSRC(uint32_t ssrc) {
    m_ssrc.store(ssrc, std::memory_order_relaxed);
}

uint32_t RTPSender::ssrc() const {
    return m_ssrc.load(std::memory_order_relaxed);
}

void RTPSender::setSequence(uint16_t sequence) {
    m_sequence.store(sequence, std::memory_order_relaxed);
}

uint16_t RTPSender::sequence() const {
    return m_sequence.load(std::memory_order_relaxed);
}

std::vector<uint8_t> RTPSender::buildPacket(uint8_t payloadType,
                                            bool marker,
                                            uint32_t timestamp,
                                            const uint8_t* payload,
                                            std::size_t payloadLen) {
    if (payload == nullptr && payloadLen != 0) {
        return {};
    }

    std::vector<uint8_t> packet(kRtpMinHeaderSize + payloadLen, 0);

    packet[0] = 0x80;  // V=2, no padding/extension/csrc
    packet[1] = static_cast<uint8_t>(payloadType & 0x7F);
    if (marker) {
        packet[1] |= 0x80;
    }

    const uint16_t seq = m_sequence.fetch_add(1, std::memory_order_relaxed);
    writeBE16(packet.data() + 2, seq);
    writeBE32(packet.data() + 4, timestamp);
    writeBE32(packet.data() + 8, ssrc());

    if (payloadLen > 0) {
        std::copy(payload, payload + payloadLen, packet.begin() + static_cast<long long>(kRtpMinHeaderSize));
    }

    return packet;
}

std::vector<uint8_t> RTPSender::buildPacket(uint8_t payloadType,
                                            bool marker,
                                            uint32_t timestamp,
                                            const std::vector<uint8_t>& payload) {
    return buildPacket(payloadType, marker, timestamp, payload.data(), payload.size());
}

}  // namespace media
