#include "H264RtpPayload.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace media {
namespace {

struct NaluView {
    const uint8_t* data{nullptr};
    std::size_t size{0};
};

std::vector<NaluView> splitAnnexBNalus(const std::vector<uint8_t>& payload) {
    std::vector<NaluView> nalus;
    if (payload.empty()) {
        return nalus;
    }

    auto findStartCode = [&payload](std::size_t from) -> std::pair<std::size_t, std::size_t> {
        for (std::size_t i = from; i + 3 < payload.size(); ++i) {
            if (payload[i] == 0x00 && payload[i + 1] == 0x00) {
                if (payload[i + 2] == 0x01) {
                    return {i, 3};
                }
                if (i + 3 < payload.size() && payload[i + 2] == 0x00 && payload[i + 3] == 0x01) {
                    return {i, 4};
                }
            }
        }
        return {payload.size(), 0};
    };

    auto [start, prefix] = findStartCode(0);
    if (prefix == 0) {
        nalus.push_back({payload.data(), payload.size()});
        return nalus;
    }

    while (prefix != 0) {
        const std::size_t naluStart = start + prefix;
        auto [nextStart, nextPrefix] = findStartCode(naluStart);
        const std::size_t naluEnd = nextPrefix == 0 ? payload.size() : nextStart;
        if (naluEnd > naluStart) {
            nalus.push_back({payload.data() + naluStart, naluEnd - naluStart});
        }
        start = nextStart;
        prefix = nextPrefix;
    }

    return nalus;
}

}  // namespace

std::vector<std::vector<uint8_t>> packetizeH264AnnexB(const std::vector<uint8_t>& encodedFrame,
                                                      std::size_t maxPayloadBytes) {
    std::vector<std::vector<uint8_t>> packets;
    const std::vector<NaluView> nalus = splitAnnexBNalus(encodedFrame);
    for (const NaluView& nalu : nalus) {
        if (nalu.data == nullptr || nalu.size == 0) {
            continue;
        }

        if (nalu.size <= maxPayloadBytes) {
            packets.emplace_back(nalu.data, nalu.data + nalu.size);
            continue;
        }

        const uint8_t nalHeader = nalu.data[0];
        const uint8_t fuIndicator = static_cast<uint8_t>((nalHeader & 0xE0) | 28);
        const uint8_t fuHeaderBase = static_cast<uint8_t>(nalHeader & 0x1F);
        std::size_t offset = 1;
        const std::size_t fragmentBytes = maxPayloadBytes > 2 ? maxPayloadBytes - 2 : 0;
        if (fragmentBytes == 0) {
            continue;
        }

        while (offset < nalu.size) {
            const std::size_t remaining = nalu.size - offset;
            const std::size_t chunk = std::min(fragmentBytes, remaining);
            std::vector<uint8_t> packet;
            packet.reserve(chunk + 2);
            packet.push_back(fuIndicator);

            uint8_t fuHeader = fuHeaderBase;
            if (offset == 1) {
                fuHeader |= 0x80;
            }
            if (offset + chunk >= nalu.size) {
                fuHeader |= 0x40;
            }
            packet.push_back(fuHeader);
            packet.insert(packet.end(),
                          nalu.data + static_cast<std::ptrdiff_t>(offset),
                          nalu.data + static_cast<std::ptrdiff_t>(offset + chunk));
            packets.push_back(std::move(packet));
            offset += chunk;
        }
    }
    return packets;
}

void H264AccessUnitAssembler::appendStartCode(std::vector<uint8_t>& target) {
    static constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
    target.insert(target.end(), std::begin(kStartCode), std::end(kStartCode));
}

void H264AccessUnitAssembler::appendCompleteNalu(std::vector<uint8_t>& target,
                                                 const uint8_t* data,
                                                 std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    appendStartCode(target);
    target.insert(target.end(), data, data + static_cast<std::ptrdiff_t>(size));
}

bool H264AccessUnitAssembler::consume(const RTPPacket& packet,
                                      H264AccessUnit& outAccessUnit,
                                      bool* packetLossDetected) {
    bool packetLoss = false;

    if (m_hasExpectedSequence && packet.header.sequenceNumber != m_expectedSequence) {
        packetLoss = true;
        m_accessUnitCorrupted = true;
        m_fragmentedNalu.clear();
        if (!m_hasTimestamp || packet.header.timestamp != m_timestamp) {
            m_accessUnit.clear();
        }
    }
    m_expectedSequence = static_cast<uint16_t>(packet.header.sequenceNumber + 1U);
    m_hasExpectedSequence = true;

    if (!m_hasTimestamp || packet.header.timestamp != m_timestamp) {
        if (m_hasTimestamp && (!m_accessUnit.empty() || !m_fragmentedNalu.empty())) {
            packetLoss = true;
        }
        m_fragmentedNalu.clear();
        m_accessUnit.clear();
        m_timestamp = packet.header.timestamp;
        m_hasTimestamp = true;
        m_accessUnitCorrupted = false;
    }

    if (packet.payload.empty()) {
        if (packetLossDetected != nullptr) {
            *packetLossDetected = packetLoss;
        }
        return false;
    }

    const uint8_t nalType = static_cast<uint8_t>(packet.payload[0] & 0x1F);
    if (nalType == 28) {
        if (packet.payload.size() < 2) {
            m_accessUnitCorrupted = true;
            if (packetLossDetected != nullptr) {
                *packetLossDetected = true;
            }
            return false;
        }

        const uint8_t fuIndicator = packet.payload[0];
        const uint8_t fuHeader = packet.payload[1];
        const bool start = (fuHeader & 0x80) != 0;
        const bool end = (fuHeader & 0x40) != 0;
        const uint8_t reconstructedHeader = static_cast<uint8_t>((fuIndicator & 0xE0) | (fuHeader & 0x1F));

        if (start) {
            m_fragmentedNalu.clear();
            m_fragmentedNalu.push_back(reconstructedHeader);
        }
        if (m_fragmentedNalu.empty()) {
            m_accessUnitCorrupted = true;
            if (packetLossDetected != nullptr) {
                *packetLossDetected = true;
            }
            return false;
        }

        m_fragmentedNalu.insert(m_fragmentedNalu.end(), packet.payload.begin() + 2, packet.payload.end());
        if (end) {
            appendCompleteNalu(m_accessUnit, m_fragmentedNalu.data(), m_fragmentedNalu.size());
            m_fragmentedNalu.clear();
        }
    } else {
        if (!m_fragmentedNalu.empty()) {
            m_accessUnitCorrupted = true;
            m_fragmentedNalu.clear();
        }
        appendCompleteNalu(m_accessUnit, packet.payload.data(), packet.payload.size());
    }

    if (!packet.header.marker || m_accessUnit.empty()) {
        if (packetLossDetected != nullptr) {
            *packetLossDetected = packetLoss;
        }
        return false;
    }

    const bool incompleteFragment = !m_fragmentedNalu.empty();
    const bool corrupted = m_accessUnitCorrupted || incompleteFragment;
    if (packetLossDetected != nullptr) {
        *packetLossDetected = packetLoss || corrupted;
    }
    if (corrupted) {
        m_accessUnit.clear();
        m_fragmentedNalu.clear();
        m_accessUnitCorrupted = false;
        return false;
    }

    outAccessUnit.payload = std::move(m_accessUnit);
    outAccessUnit.pts = static_cast<int64_t>(packet.header.timestamp);
    outAccessUnit.payloadType = packet.header.payloadType;
    m_accessUnit.clear();
    m_fragmentedNalu.clear();
    m_accessUnitCorrupted = false;
    return !outAccessUnit.payload.empty();
}

}  // namespace media
