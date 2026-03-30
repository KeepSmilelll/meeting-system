#include <cassert>
#include <cstdint>
#include <vector>

#include "../src/net/signaling/SignalProtocol.h"

int main() {
    const uint16_t type = 0x0105;
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};

    const auto frame = signaling::encodeFrame(type, payload);
    assert(frame.size() == signaling::kHeaderSize + payload.size());

    const std::vector<uint8_t> header(frame.begin(), frame.begin() + static_cast<long long>(signaling::kHeaderSize));
    const auto decoded = signaling::decodeHeader(header, 1024);
    assert(decoded.has_value());
    assert(decoded->type == type);
    assert(decoded->length == payload.size());

    const auto tooSmall = signaling::decodeHeader(header, 2);
    assert(!tooSmall.has_value());

    return 0;
}
