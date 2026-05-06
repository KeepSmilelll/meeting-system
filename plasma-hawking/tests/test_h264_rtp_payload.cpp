#include "net/media/H264RtpPayload.h"

#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::vector<uint8_t> makeLargeAnnexBNalu() {
    std::vector<uint8_t> encoded{0x00, 0x00, 0x00, 0x01, 0x65};
    for (uint8_t i = 1; i <= 24; ++i) {
        encoded.push_back(i);
    }
    return encoded;
}

media::RTPPacket makePacket(uint16_t sequence,
                            bool marker,
                            const std::vector<uint8_t>& payload,
                            uint32_t timestamp = 90000U) {
    media::RTPPacket packet;
    packet.header.sequenceNumber = sequence;
    packet.header.timestamp = timestamp;
    packet.header.payloadType = 96;
    packet.header.marker = marker;
    packet.payload = payload;
    return packet;
}

bool testFragmentedRoundTrip() {
    const std::vector<uint8_t> encoded = makeLargeAnnexBNalu();
    const auto payloads = media::packetizeH264AnnexB(encoded, 6);
    if (!expect(payloads.size() > 2, "expected FU-A fragmentation")) {
        return false;
    }

    media::H264AccessUnitAssembler assembler;
    media::H264AccessUnit accessUnit;
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        bool packetLoss = false;
        const media::RTPPacket packet = makePacket(static_cast<uint16_t>(100 + i),
                                                   i + 1 == payloads.size(),
                                                   payloads[i]);
        const bool ready = assembler.consume(packet, accessUnit, &packetLoss);
        if (!expect(!packetLoss, "unexpected packet loss during ordered round trip")) {
            return false;
        }
        if (i + 1 < payloads.size()) {
            if (!expect(!ready, "access unit completed before marker packet")) {
                return false;
            }
        } else if (!expect(ready, "access unit did not complete on marker packet")) {
            return false;
        }
    }

    return expect(accessUnit.payload == encoded, "reassembled Annex-B payload differs") &&
           expect(accessUnit.pts == 90000, "RTP timestamp was not preserved") &&
           expect(accessUnit.payloadType == 96, "payload type was not preserved");
}

bool testFragmentLossDropsAccessUnit() {
    const std::vector<uint8_t> encoded = makeLargeAnnexBNalu();
    const auto payloads = media::packetizeH264AnnexB(encoded, 6);
    if (!expect(payloads.size() > 3, "expected enough fragments for loss test")) {
        return false;
    }

    media::H264AccessUnitAssembler assembler;
    media::H264AccessUnit accessUnit;
    bool packetLoss = false;
    (void)assembler.consume(makePacket(200, false, payloads[0]), accessUnit, &packetLoss);
    if (!expect(!packetLoss, "first packet should not be marked lost")) {
        return false;
    }

    bool completed = false;
    for (std::size_t i = 2; i < payloads.size(); ++i) {
        packetLoss = false;
        completed = assembler.consume(makePacket(static_cast<uint16_t>(200 + i),
                                                 i + 1 == payloads.size(),
                                                 payloads[i]),
                                      accessUnit,
                                      &packetLoss);
    }

    return expect(packetLoss, "missing FU-A fragment was not detected") &&
           expect(!completed, "corrupted access unit should be dropped");
}

bool testFragmentLossReportsMissingSequence() {
    const std::vector<uint8_t> encoded = makeLargeAnnexBNalu();
    const auto payloads = media::packetizeH264AnnexB(encoded, 6);
    if (!expect(payloads.size() > 3, "expected enough fragments for missing sequence test")) {
        return false;
    }

    media::H264AccessUnitAssembler assembler;
    media::H264AccessUnit accessUnit;
    bool packetLoss = false;
    media::H264PacketLossInfo lossInfo;
    (void)assembler.consume(makePacket(500, false, payloads[0]), accessUnit, &packetLoss, &lossInfo);
    if (!expect(!packetLoss && lossInfo.missingSequences.empty(), "first packet should not report loss")) {
        return false;
    }

    const bool completed = assembler.consume(makePacket(502, false, payloads[2]),
                                             accessUnit,
                                             &packetLoss,
                                             &lossInfo);
    return expect(packetLoss, "sequence gap was not reported as packet loss") &&
           expect(!completed, "gap packet should not complete access unit") &&
           expect(lossInfo.missingSequences.size() == 1U, "expected exactly one missing sequence") &&
           expect(lossInfo.missingSequences.front() == 501U, "wrong missing sequence reported");
}

bool testLatePacketDoesNotReportForwardLoss() {
    media::H264AccessUnitAssembler assembler;
    media::H264AccessUnit accessUnit;
    bool packetLoss = false;
    media::H264PacketLossInfo lossInfo;

    const std::vector<uint8_t> nalu{0x65, 0x01, 0x02};
    (void)assembler.consume(makePacket(700, true, nalu), accessUnit, &packetLoss, &lossInfo);
    if (!expect(!packetLoss && lossInfo.missingSequences.empty(), "ordered packet should not report loss")) {
        return false;
    }

    const bool completed = assembler.consume(makePacket(700, true, nalu),
                                             accessUnit,
                                             &packetLoss,
                                             &lossInfo);
    return expect(!completed, "duplicate packet should be ignored") &&
           expect(!packetLoss, "duplicate packet should not report packet loss") &&
           expect(lossInfo.missingSequences.empty(), "duplicate packet should not report missing sequences");
}

}  // namespace

int main() {
    if (!testFragmentedRoundTrip()) {
        return 1;
    }
    if (!testFragmentLossDropsAccessUnit()) {
        return 1;
    }
    if (!testFragmentLossReportsMissingSequence()) {
        return 1;
    }
    if (!testLatePacketDoesNotReportForwardLoss()) {
        return 1;
    }
    return 0;
}
