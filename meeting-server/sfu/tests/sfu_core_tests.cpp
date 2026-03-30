#include "rtp/NackBuffer.h"
#include "rtp/RtpParser.h"
#include "rtp/RtpRouter.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool TestRtpParserValidPacket() {
    std::vector<uint8_t> packet = {
        0x80, 0x60,       // V=2, PT=96
        0x00, 0x2A,       // Seq=42
        0x00, 0x00, 0x00, 0x64, // TS=100
        0x11, 0x11, 0x11, 0x11, // SSRC=0x11111111
        0xAA, 0xBB, 0xCC
    };

    sfu::ParsedRtpPacket parsed;
    sfu::RtpParser parser;
    if (!parser.Parse(packet.data(), packet.size(), &parsed)) {
        std::cerr << "RtpParser should parse valid RTP packet\n";
        return false;
    }

    return parsed.header.version == 2 &&
           parsed.header.payloadType == 96 &&
           parsed.header.sequence == 42 &&
           parsed.header.ssrc == 0x11111111U &&
           parsed.payloadSize == 3;
}

bool TestRtpParserRejectInvalidVersion() {
    std::vector<uint8_t> packet = {
        0x40, 0x60, // V=1 (invalid)
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x11, 0x11, 0x11, 0x11,
    };

    sfu::ParsedRtpPacket parsed;
    sfu::RtpParser parser;
    return !parser.Parse(packet.data(), packet.size(), &parsed);
}

bool TestNackBufferCapacity() {
    sfu::NackBuffer buffer(2);

    const std::vector<uint8_t> p1 = {1, 2, 3};
    const std::vector<uint8_t> p2 = {4, 5, 6};
    const std::vector<uint8_t> p3 = {7, 8, 9};

    buffer.Store(1, p1.data(), p1.size());
    buffer.Store(2, p2.data(), p2.size());
    buffer.Store(3, p3.data(), p3.size()); // should evict seq=1

    std::vector<uint8_t> out;
    if (buffer.Get(1, &out)) {
        std::cerr << "Oldest packet should be evicted when capacity exceeded\n";
        return false;
    }

    return buffer.Get(2, &out) && buffer.Get(3, &out) && buffer.Size() == 2;
}

bool TestRtpRouterForwardAndRetransmit() {
    sfu::RtpRouter router(8);
    if (!router.RegisterPublisher(0x11111111U)) {
        std::cerr << "RegisterPublisher failed\n";
        return false;
    }

    bool forwarded = false;
    std::vector<uint8_t> forwardedPacket;

    if (!router.AddSubscriber(0x11111111U, 0x22222222U,
        [&forwarded, &forwardedPacket](const uint8_t* data, std::size_t len) {
            forwarded = true;
            forwardedPacket.assign(data, data + len);
        })) {
        std::cerr << "AddSubscriber failed\n";
        return false;
    }

    std::vector<uint8_t> packet = {
        0x80, 0x60,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD
    };

    if (!router.Route(packet.data(), packet.size())) {
        std::cerr << "Route failed\n";
        return false;
    }

    if (!forwarded || forwardedPacket.size() != packet.size()) {
        std::cerr << "Forward callback not invoked as expected\n";
        return false;
    }

    // rewritten SSRC should be 0x22222222
    if (forwardedPacket[8] != 0x22 || forwardedPacket[9] != 0x22 ||
        forwardedPacket[10] != 0x22 || forwardedPacket[11] != 0x22) {
        std::cerr << "Forwarded packet SSRC rewrite mismatch\n";
        return false;
    }

    std::vector<uint8_t> retransmit;
    if (!router.GetRetransmitPacket(0x11111111U, 42, &retransmit)) {
        std::cerr << "GetRetransmitPacket failed\n";
        return false;
    }

    return retransmit == packet;
}

} // namespace

int main() {
    if (!TestRtpParserValidPacket()) {
        return 1;
    }
    if (!TestRtpParserRejectInvalidVersion()) {
        return 1;
    }
    if (!TestNackBufferCapacity()) {
        return 1;
    }
    if (!TestRtpRouterForwardAndRetransmit()) {
        return 1;
    }

    std::cout << "sfu_core_tests passed\n";
    return 0;
}
