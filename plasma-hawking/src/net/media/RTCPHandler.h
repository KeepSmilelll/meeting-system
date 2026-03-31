#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

struct RTCPHeader {
    uint8_t version{0};
    bool padding{false};
    uint8_t countOrFormat{0};
    uint8_t packetType{0};
    uint16_t lengthWords{0};
};

struct RTCPPacketSlice {
    RTCPHeader header;
    std::size_t offset{0};
    std::size_t size{0};
};

struct RTCPReportBlock {
    uint32_t sourceSsrc{0};
    uint8_t fractionLost{0};
    int32_t cumulativeLost{0};
    uint32_t highestSeq{0};
    uint32_t jitter{0};
    uint32_t lastSenderReport{0};
    uint32_t delaySinceLastSenderReport{0};
};

struct RTCPSenderReport {
    uint32_t senderSsrc{0};
    uint64_t ntpTimestamp{0};
    uint32_t rtpTimestamp{0};
    uint32_t packetCount{0};
    uint32_t octetCount{0};
    std::vector<RTCPReportBlock> reportBlocks;
};

struct RTCPReceiverReport {
    uint32_t receiverSsrc{0};
    std::vector<RTCPReportBlock> reportBlocks;
};

class RTCPHandler {
public:
    bool parseHeader(const uint8_t* data, std::size_t len, RTCPHeader& outHeader) const;
    std::size_t packetSizeBytes(const RTCPHeader& header) const;

    bool parsePacketSlice(const uint8_t* data, std::size_t len, RTCPPacketSlice& outSlice) const;
    bool parseCompoundPacket(const uint8_t* data, std::size_t len, std::vector<RTCPPacketSlice>& outSlices) const;

    bool parseSenderReport(const uint8_t* data, std::size_t len, RTCPSenderReport& outReport) const;
    bool parseReceiverReport(const uint8_t* data, std::size_t len, RTCPReceiverReport& outReport) const;

    std::vector<uint8_t> buildSenderReport(const RTCPSenderReport& report) const;
    std::vector<uint8_t> buildReceiverReport(const RTCPReceiverReport& report) const;

    bool isReceiverReport(const RTCPHeader& header) const;
    bool isSenderReport(const RTCPHeader& header) const;
    bool isFeedbackPacket(const RTCPHeader& header) const;
};

bool runRtcpMainFlowSelfCheck();

}  // namespace media

inline bool media::RTCPHandler::parseHeader(const uint8_t* data, std::size_t len, RTCPHeader& outHeader) const {
    if (data == nullptr || len < 4) {
        return false;
    }

    const uint8_t b0 = data[0];
    outHeader.version = static_cast<uint8_t>((b0 >> 6) & 0x03);
    outHeader.padding = (b0 & 0x20) != 0;
    outHeader.countOrFormat = static_cast<uint8_t>(b0 & 0x1F);
    outHeader.packetType = data[1];
    outHeader.lengthWords = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]);

    if (outHeader.version != 2) {
        return false;
    }

    const std::size_t packetBytes = packetSizeBytes(outHeader);
    if (packetBytes < 4 || packetBytes > len) {
        return false;
    }

    return true;
}

inline std::size_t media::RTCPHandler::packetSizeBytes(const RTCPHeader& header) const {
    return static_cast<std::size_t>(header.lengthWords + 1U) * 4U;
}

inline bool media::RTCPHandler::parsePacketSlice(const uint8_t* data, std::size_t len, RTCPPacketSlice& outSlice) const {
    if (!parseHeader(data, len, outSlice.header)) {
        return false;
    }

    outSlice.offset = 0;
    outSlice.size = packetSizeBytes(outSlice.header);
    return outSlice.size <= len;
}

inline bool media::RTCPHandler::parseCompoundPacket(const uint8_t* data, std::size_t len, std::vector<RTCPPacketSlice>& outSlices) const {
    outSlices.clear();
    if (data == nullptr || len == 0) {
        return false;
    }

    std::size_t offset = 0;
    while (offset < len) {
        RTCPPacketSlice slice{};
        if (!parsePacketSlice(data + offset, len - offset, slice)) {
            outSlices.clear();
            return false;
        }
        slice.offset = offset;
        outSlices.push_back(slice);
        offset += slice.size;
    }
    return !outSlices.empty();
}

inline bool media::RTCPHandler::parseSenderReport(const uint8_t* data, std::size_t len, RTCPSenderReport& outReport) const {
    RTCPHeader header{};
    if (!parseHeader(data, len, header) || !isSenderReport(header)) {
        return false;
    }

    const std::size_t packetBytes = packetSizeBytes(header);
    if (packetBytes < 28 || len < packetBytes) {
        return false;
    }

    const uint8_t* body = data + 4;
    outReport.senderSsrc = (static_cast<uint32_t>(body[0]) << 24) |
                           (static_cast<uint32_t>(body[1]) << 16) |
                           (static_cast<uint32_t>(body[2]) << 8) |
                           static_cast<uint32_t>(body[3]);
    const uint64_t ntpMsw = (static_cast<uint64_t>(body[4]) << 24) |
                            (static_cast<uint64_t>(body[5]) << 16) |
                            (static_cast<uint64_t>(body[6]) << 8) |
                            static_cast<uint64_t>(body[7]);
    const uint64_t ntpLsw = (static_cast<uint64_t>(body[8]) << 24) |
                            (static_cast<uint64_t>(body[9]) << 16) |
                            (static_cast<uint64_t>(body[10]) << 8) |
                            static_cast<uint64_t>(body[11]);
    outReport.ntpTimestamp = (ntpMsw << 32) | ntpLsw;
    outReport.rtpTimestamp = (static_cast<uint32_t>(body[12]) << 24) |
                             (static_cast<uint32_t>(body[13]) << 16) |
                             (static_cast<uint32_t>(body[14]) << 8) |
                             static_cast<uint32_t>(body[15]);
    outReport.packetCount = (static_cast<uint32_t>(body[16]) << 24) |
                            (static_cast<uint32_t>(body[17]) << 16) |
                            (static_cast<uint32_t>(body[18]) << 8) |
                            static_cast<uint32_t>(body[19]);
    outReport.octetCount = (static_cast<uint32_t>(body[20]) << 24) |
                           (static_cast<uint32_t>(body[21]) << 16) |
                           (static_cast<uint32_t>(body[22]) << 8) |
                           static_cast<uint32_t>(body[23]);

    outReport.reportBlocks.clear();
    outReport.reportBlocks.reserve(header.countOrFormat);
    const uint8_t* blockPtr = body + 24;
    for (uint8_t i = 0; i < header.countOrFormat; ++i) {
        if (blockPtr + 24 > data + packetBytes) {
            return false;
        }
        RTCPReportBlock block{};
        block.sourceSsrc = (static_cast<uint32_t>(blockPtr[0]) << 24) |
                           (static_cast<uint32_t>(blockPtr[1]) << 16) |
                           (static_cast<uint32_t>(blockPtr[2]) << 8) |
                           static_cast<uint32_t>(blockPtr[3]);
        block.fractionLost = blockPtr[4];
        int32_t cumulativeLost = (static_cast<int32_t>(blockPtr[5]) << 16) |
                                 (static_cast<int32_t>(blockPtr[6]) << 8) |
                                 static_cast<int32_t>(blockPtr[7]);
        if ((cumulativeLost & 0x00800000) != 0) {
            cumulativeLost |= ~0x00FFFFFF;
        }
        block.cumulativeLost = cumulativeLost;
        block.highestSeq = (static_cast<uint32_t>(blockPtr[8]) << 24) |
                           (static_cast<uint32_t>(blockPtr[9]) << 16) |
                           (static_cast<uint32_t>(blockPtr[10]) << 8) |
                           static_cast<uint32_t>(blockPtr[11]);
        block.jitter = (static_cast<uint32_t>(blockPtr[12]) << 24) |
                       (static_cast<uint32_t>(blockPtr[13]) << 16) |
                       (static_cast<uint32_t>(blockPtr[14]) << 8) |
                       static_cast<uint32_t>(blockPtr[15]);
        block.lastSenderReport = (static_cast<uint32_t>(blockPtr[16]) << 24) |
                                 (static_cast<uint32_t>(blockPtr[17]) << 16) |
                                 (static_cast<uint32_t>(blockPtr[18]) << 8) |
                                 static_cast<uint32_t>(blockPtr[19]);
        block.delaySinceLastSenderReport = (static_cast<uint32_t>(blockPtr[20]) << 24) |
                                           (static_cast<uint32_t>(blockPtr[21]) << 16) |
                                           (static_cast<uint32_t>(blockPtr[22]) << 8) |
                                           static_cast<uint32_t>(blockPtr[23]);
        outReport.reportBlocks.push_back(block);
        blockPtr += 24;
    }
    return true;
}

inline bool media::RTCPHandler::parseReceiverReport(const uint8_t* data, std::size_t len, RTCPReceiverReport& outReport) const {
    RTCPHeader header{};
    if (!parseHeader(data, len, header) || !isReceiverReport(header)) {
        return false;
    }

    const std::size_t packetBytes = packetSizeBytes(header);
    if (packetBytes < 8 || len < packetBytes) {
        return false;
    }

    const uint8_t* body = data + 4;
    outReport.receiverSsrc = (static_cast<uint32_t>(body[0]) << 24) |
                             (static_cast<uint32_t>(body[1]) << 16) |
                             (static_cast<uint32_t>(body[2]) << 8) |
                             static_cast<uint32_t>(body[3]);
    outReport.reportBlocks.clear();
    outReport.reportBlocks.reserve(header.countOrFormat);

    const uint8_t* blockPtr = body + 4;
    for (uint8_t i = 0; i < header.countOrFormat; ++i) {
        if (blockPtr + 24 > data + packetBytes) {
            return false;
        }
        RTCPReportBlock block{};
        block.sourceSsrc = (static_cast<uint32_t>(blockPtr[0]) << 24) |
                           (static_cast<uint32_t>(blockPtr[1]) << 16) |
                           (static_cast<uint32_t>(blockPtr[2]) << 8) |
                           static_cast<uint32_t>(blockPtr[3]);
        block.fractionLost = blockPtr[4];
        int32_t cumulativeLost = (static_cast<int32_t>(blockPtr[5]) << 16) |
                                 (static_cast<int32_t>(blockPtr[6]) << 8) |
                                 static_cast<int32_t>(blockPtr[7]);
        if ((cumulativeLost & 0x00800000) != 0) {
            cumulativeLost |= ~0x00FFFFFF;
        }
        block.cumulativeLost = cumulativeLost;
        block.highestSeq = (static_cast<uint32_t>(blockPtr[8]) << 24) |
                           (static_cast<uint32_t>(blockPtr[9]) << 16) |
                           (static_cast<uint32_t>(blockPtr[10]) << 8) |
                           static_cast<uint32_t>(blockPtr[11]);
        block.jitter = (static_cast<uint32_t>(blockPtr[12]) << 24) |
                       (static_cast<uint32_t>(blockPtr[13]) << 16) |
                       (static_cast<uint32_t>(blockPtr[14]) << 8) |
                       static_cast<uint32_t>(blockPtr[15]);
        block.lastSenderReport = (static_cast<uint32_t>(blockPtr[16]) << 24) |
                                 (static_cast<uint32_t>(blockPtr[17]) << 16) |
                                 (static_cast<uint32_t>(blockPtr[18]) << 8) |
                                 static_cast<uint32_t>(blockPtr[19]);
        block.delaySinceLastSenderReport = (static_cast<uint32_t>(blockPtr[20]) << 24) |
                                           (static_cast<uint32_t>(blockPtr[21]) << 16) |
                                           (static_cast<uint32_t>(blockPtr[22]) << 8) |
                                           static_cast<uint32_t>(blockPtr[23]);
        outReport.reportBlocks.push_back(block);
        blockPtr += 24;
    }
    return true;
}

inline std::vector<uint8_t> media::RTCPHandler::buildSenderReport(const RTCPSenderReport& report) const {
    if (report.reportBlocks.size() > 31) {
        return {};
    }

    const std::size_t packetBytes = 28U + report.reportBlocks.size() * 24U;
    std::vector<uint8_t> packet(packetBytes, 0);
    packet[0] = static_cast<uint8_t>((2U << 6) | static_cast<uint8_t>(report.reportBlocks.size() & 0x1FU));
    packet[1] = 200;
    const uint16_t lengthWords = static_cast<uint16_t>((packetBytes / 4U) - 1U);
    packet[2] = static_cast<uint8_t>((lengthWords >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(lengthWords & 0xFF);

    uint8_t* body = packet.data() + 4;
    body[0] = static_cast<uint8_t>((report.senderSsrc >> 24) & 0xFF);
    body[1] = static_cast<uint8_t>((report.senderSsrc >> 16) & 0xFF);
    body[2] = static_cast<uint8_t>((report.senderSsrc >> 8) & 0xFF);
    body[3] = static_cast<uint8_t>(report.senderSsrc & 0xFF);
    body[4] = static_cast<uint8_t>((report.ntpTimestamp >> 56) & 0xFF);
    body[5] = static_cast<uint8_t>((report.ntpTimestamp >> 48) & 0xFF);
    body[6] = static_cast<uint8_t>((report.ntpTimestamp >> 40) & 0xFF);
    body[7] = static_cast<uint8_t>((report.ntpTimestamp >> 32) & 0xFF);
    body[8] = static_cast<uint8_t>((report.ntpTimestamp >> 24) & 0xFF);
    body[9] = static_cast<uint8_t>((report.ntpTimestamp >> 16) & 0xFF);
    body[10] = static_cast<uint8_t>((report.ntpTimestamp >> 8) & 0xFF);
    body[11] = static_cast<uint8_t>(report.ntpTimestamp & 0xFF);
    body[12] = static_cast<uint8_t>((report.rtpTimestamp >> 24) & 0xFF);
    body[13] = static_cast<uint8_t>((report.rtpTimestamp >> 16) & 0xFF);
    body[14] = static_cast<uint8_t>((report.rtpTimestamp >> 8) & 0xFF);
    body[15] = static_cast<uint8_t>(report.rtpTimestamp & 0xFF);
    body[16] = static_cast<uint8_t>((report.packetCount >> 24) & 0xFF);
    body[17] = static_cast<uint8_t>((report.packetCount >> 16) & 0xFF);
    body[18] = static_cast<uint8_t>((report.packetCount >> 8) & 0xFF);
    body[19] = static_cast<uint8_t>(report.packetCount & 0xFF);
    body[20] = static_cast<uint8_t>((report.octetCount >> 24) & 0xFF);
    body[21] = static_cast<uint8_t>((report.octetCount >> 16) & 0xFF);
    body[22] = static_cast<uint8_t>((report.octetCount >> 8) & 0xFF);
    body[23] = static_cast<uint8_t>(report.octetCount & 0xFF);

    uint8_t* blockPtr = body + 24;
    for (const RTCPReportBlock& block : report.reportBlocks) {
        blockPtr[0] = static_cast<uint8_t>((block.sourceSsrc >> 24) & 0xFF);
        blockPtr[1] = static_cast<uint8_t>((block.sourceSsrc >> 16) & 0xFF);
        blockPtr[2] = static_cast<uint8_t>((block.sourceSsrc >> 8) & 0xFF);
        blockPtr[3] = static_cast<uint8_t>(block.sourceSsrc & 0xFF);
        blockPtr[4] = block.fractionLost;
        const uint32_t lost = static_cast<uint32_t>(block.cumulativeLost) & 0x00FFFFFFU;
        blockPtr[5] = static_cast<uint8_t>((lost >> 16) & 0xFF);
        blockPtr[6] = static_cast<uint8_t>((lost >> 8) & 0xFF);
        blockPtr[7] = static_cast<uint8_t>(lost & 0xFF);
        blockPtr[8] = static_cast<uint8_t>((block.highestSeq >> 24) & 0xFF);
        blockPtr[9] = static_cast<uint8_t>((block.highestSeq >> 16) & 0xFF);
        blockPtr[10] = static_cast<uint8_t>((block.highestSeq >> 8) & 0xFF);
        blockPtr[11] = static_cast<uint8_t>(block.highestSeq & 0xFF);
        blockPtr[12] = static_cast<uint8_t>((block.jitter >> 24) & 0xFF);
        blockPtr[13] = static_cast<uint8_t>((block.jitter >> 16) & 0xFF);
        blockPtr[14] = static_cast<uint8_t>((block.jitter >> 8) & 0xFF);
        blockPtr[15] = static_cast<uint8_t>(block.jitter & 0xFF);
        blockPtr[16] = static_cast<uint8_t>((block.lastSenderReport >> 24) & 0xFF);
        blockPtr[17] = static_cast<uint8_t>((block.lastSenderReport >> 16) & 0xFF);
        blockPtr[18] = static_cast<uint8_t>((block.lastSenderReport >> 8) & 0xFF);
        blockPtr[19] = static_cast<uint8_t>(block.lastSenderReport & 0xFF);
        blockPtr[20] = static_cast<uint8_t>((block.delaySinceLastSenderReport >> 24) & 0xFF);
        blockPtr[21] = static_cast<uint8_t>((block.delaySinceLastSenderReport >> 16) & 0xFF);
        blockPtr[22] = static_cast<uint8_t>((block.delaySinceLastSenderReport >> 8) & 0xFF);
        blockPtr[23] = static_cast<uint8_t>(block.delaySinceLastSenderReport & 0xFF);
        blockPtr += 24;
    }

    return packet;
}

inline std::vector<uint8_t> media::RTCPHandler::buildReceiverReport(const RTCPReceiverReport& report) const {
    if (report.reportBlocks.size() > 31) {
        return {};
    }

    const std::size_t packetBytes = 8U + report.reportBlocks.size() * 24U;
    std::vector<uint8_t> packet(packetBytes, 0);
    packet[0] = static_cast<uint8_t>((2U << 6) | static_cast<uint8_t>(report.reportBlocks.size() & 0x1FU));
    packet[1] = 201;
    const uint16_t lengthWords = static_cast<uint16_t>((packetBytes / 4U) - 1U);
    packet[2] = static_cast<uint8_t>((lengthWords >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(lengthWords & 0xFF);

    uint8_t* body = packet.data() + 4;
    body[0] = static_cast<uint8_t>((report.receiverSsrc >> 24) & 0xFF);
    body[1] = static_cast<uint8_t>((report.receiverSsrc >> 16) & 0xFF);
    body[2] = static_cast<uint8_t>((report.receiverSsrc >> 8) & 0xFF);
    body[3] = static_cast<uint8_t>(report.receiverSsrc & 0xFF);

    uint8_t* blockPtr = body + 4;
    for (const RTCPReportBlock& block : report.reportBlocks) {
        blockPtr[0] = static_cast<uint8_t>((block.sourceSsrc >> 24) & 0xFF);
        blockPtr[1] = static_cast<uint8_t>((block.sourceSsrc >> 16) & 0xFF);
        blockPtr[2] = static_cast<uint8_t>((block.sourceSsrc >> 8) & 0xFF);
        blockPtr[3] = static_cast<uint8_t>(block.sourceSsrc & 0xFF);
        blockPtr[4] = block.fractionLost;
        const uint32_t lost = static_cast<uint32_t>(block.cumulativeLost) & 0x00FFFFFFU;
        blockPtr[5] = static_cast<uint8_t>((lost >> 16) & 0xFF);
        blockPtr[6] = static_cast<uint8_t>((lost >> 8) & 0xFF);
        blockPtr[7] = static_cast<uint8_t>(lost & 0xFF);
        blockPtr[8] = static_cast<uint8_t>((block.highestSeq >> 24) & 0xFF);
        blockPtr[9] = static_cast<uint8_t>((block.highestSeq >> 16) & 0xFF);
        blockPtr[10] = static_cast<uint8_t>((block.highestSeq >> 8) & 0xFF);
        blockPtr[11] = static_cast<uint8_t>(block.highestSeq & 0xFF);
        blockPtr[12] = static_cast<uint8_t>((block.jitter >> 24) & 0xFF);
        blockPtr[13] = static_cast<uint8_t>((block.jitter >> 16) & 0xFF);
        blockPtr[14] = static_cast<uint8_t>((block.jitter >> 8) & 0xFF);
        blockPtr[15] = static_cast<uint8_t>(block.jitter & 0xFF);
        blockPtr[16] = static_cast<uint8_t>((block.lastSenderReport >> 24) & 0xFF);
        blockPtr[17] = static_cast<uint8_t>((block.lastSenderReport >> 16) & 0xFF);
        blockPtr[18] = static_cast<uint8_t>((block.lastSenderReport >> 8) & 0xFF);
        blockPtr[19] = static_cast<uint8_t>(block.lastSenderReport & 0xFF);
        blockPtr[20] = static_cast<uint8_t>((block.delaySinceLastSenderReport >> 24) & 0xFF);
        blockPtr[21] = static_cast<uint8_t>((block.delaySinceLastSenderReport >> 16) & 0xFF);
        blockPtr[22] = static_cast<uint8_t>((block.delaySinceLastSenderReport >> 8) & 0xFF);
        blockPtr[23] = static_cast<uint8_t>(block.delaySinceLastSenderReport & 0xFF);
        blockPtr += 24;
    }

    return packet;
}

inline bool media::RTCPHandler::isReceiverReport(const RTCPHeader& header) const {
    return header.packetType == 201;
}

inline bool media::RTCPHandler::isSenderReport(const RTCPHeader& header) const {
    return header.packetType == 200;
}

inline bool media::RTCPHandler::isFeedbackPacket(const RTCPHeader& header) const {
    return header.packetType == 205 || header.packetType == 206;
}

inline bool media::runRtcpMainFlowSelfCheck() {
    RTCPHandler handler;

    RTCPSenderReport sender{};
    sender.senderSsrc = 0x01020304;
    sender.ntpTimestamp = 0x1111222233334444ULL;
    sender.rtpTimestamp = 0x55556666;
    sender.packetCount = 42;
    sender.octetCount = 2048;
    sender.reportBlocks.push_back(RTCPReportBlock{
        0x0A0B0C0D,
        5,
        -2,
        0x12345678,
        9,
        0x01020304,
        0x05060708,
    });

    const auto senderBytes = handler.buildSenderReport(sender);
    if (senderBytes.empty()) {
        return false;
    }

    RTCPReceiverReport receiver{};
    receiver.receiverSsrc = 0xAABBCCDD;
    receiver.reportBlocks.push_back(RTCPReportBlock{
        0x01020304,
        7,
        3,
        0x22334455,
        12,
        0x10203040,
        0x50607080,
    });

    const auto receiverBytes = handler.buildReceiverReport(receiver);
    if (receiverBytes.empty()) {
        return false;
    }

    std::vector<uint8_t> compound;
    compound.reserve(senderBytes.size() + receiverBytes.size());
    compound.insert(compound.end(), senderBytes.begin(), senderBytes.end());
    compound.insert(compound.end(), receiverBytes.begin(), receiverBytes.end());

    std::vector<RTCPPacketSlice> slices;
    if (!handler.parseCompoundPacket(compound.data(), compound.size(), slices) || slices.size() != 2) {
        return false;
    }

    RTCPSenderReport parsedSender{};
    if (!handler.parseSenderReport(compound.data() + slices[0].offset, slices[0].size, parsedSender)) {
        return false;
    }
    if (parsedSender.senderSsrc != sender.senderSsrc ||
        parsedSender.ntpTimestamp != sender.ntpTimestamp ||
        parsedSender.reportBlocks.empty() ||
        parsedSender.reportBlocks.front().cumulativeLost != -2) {
        return false;
    }

    RTCPReceiverReport parsedReceiver{};
    if (!handler.parseReceiverReport(compound.data() + slices[1].offset, slices[1].size, parsedReceiver)) {
        return false;
    }
    if (parsedReceiver.receiverSsrc != receiver.receiverSsrc ||
        parsedReceiver.reportBlocks.empty() ||
        parsedReceiver.reportBlocks.front().fractionLost != 7) {
        return false;
    }

    RTCPHeader feedbackHeader{};
    feedbackHeader.packetType = 205;
    return handler.isSenderReport(slices[0].header) &&
           handler.isReceiverReport(slices[1].header) &&
           handler.isFeedbackPacket(feedbackHeader);
}
