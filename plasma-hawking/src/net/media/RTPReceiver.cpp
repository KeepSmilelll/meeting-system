#include "RTPReceiver.h"

#include <algorithm>

namespace media {

bool RTPReceiver::parsePacket(const uint8_t* data, std::size_t len, RTPPacket& outPacket) const {
    std::size_t payloadOffset = 0;
    if (!parseRTPHeader(data, len, outPacket.header, payloadOffset)) {
        return false;
    }

    std::size_t payloadLen = len - payloadOffset;
    if (outPacket.header.padding && payloadLen > 0) {
        const uint8_t padLen = data[len - 1];
        if (padLen == 0 || padLen > payloadLen) {
            return false;
        }
        payloadLen -= padLen;
    }

    outPacket.payload.resize(payloadLen);
    if (payloadLen > 0) {
        std::copy(data + payloadOffset, data + payloadOffset + payloadLen, outPacket.payload.begin());
    }
    return true;
}

bool RTPReceiver::parsePacket(const std::vector<uint8_t>& data, RTPPacket& outPacket) const {
    return parsePacket(data.data(), data.size(), outPacket);
}

bool runRtpLoopbackSelfCheck() {
    RTPSender sender(0x55667788, 200);
    const std::vector<uint8_t> payload{1, 3, 5, 7, 9, 11};

    const auto wire = sender.buildPacket(111, true, 960, payload);
    if (wire.empty()) {
        return false;
    }

    RTPReceiver receiver;
    RTPPacket parsed;
    if (!receiver.parsePacket(wire, parsed)) {
        return false;
    }

    return parsed.header.version == 2 &&
           parsed.header.payloadType == 111 &&
           parsed.header.ssrc == 0x55667788 &&
           parsed.payload == payload;
}

}  // namespace media
