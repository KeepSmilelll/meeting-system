#include <cassert>
#include <chrono>
#include <thread>

#include "net/media/JitterBuffer.h"

namespace {

media::RTPPacket makePacket(uint16_t sequence, uint8_t payloadByte) {
    media::RTPPacket packet;
    packet.header.version = 2;
    packet.header.payloadType = 111;
    packet.header.sequenceNumber = sequence;
    packet.header.timestamp = static_cast<uint32_t>(sequence) * 960U;
    packet.header.ssrc = 0x12345678U;
    packet.payload = {payloadByte};
    return packet;
}

}  // namespace

int main() {
    media::JitterBuffer buffer(3);

    assert(buffer.push(makePacket(10, 0x0A)));
    assert(buffer.push(makePacket(12, 0x0C)));
    assert(buffer.push(makePacket(11, 0x0B)));

    media::RTPPacket out;
    assert(buffer.pop(out));
    assert(out.header.sequenceNumber == 10);
    assert(buffer.pop(out));
    assert(out.header.sequenceNumber == 11);
    assert(buffer.pop(out));
    assert(out.header.sequenceNumber == 12);
    assert(!buffer.pop(out));

    buffer.clear();
    buffer.setMaxPackets(2);
    assert(buffer.push(makePacket(100, 1)));
    assert(buffer.push(makePacket(101, 2)));
    assert(buffer.push(makePacket(102, 3)));
    assert(buffer.size() == 2);
    assert(buffer.pop(out));
    assert(out.header.sequenceNumber == 101);

    buffer.clear();
    std::thread producer([&buffer]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        (void)buffer.push(makePacket(200, 4));
    });

    assert(buffer.popWait(out, std::chrono::milliseconds(200)));
    assert(out.header.sequenceNumber == 200);
    producer.join();

    media::JitterBuffer prebuffered(8, 3, std::chrono::milliseconds(20));
    assert(prebuffered.push(makePacket(10, 0x0A)));
    assert(!prebuffered.pop(out));
    assert(prebuffered.push(makePacket(12, 0x0C)));
    assert(!prebuffered.pop(out));
    assert(prebuffered.push(makePacket(11, 0x0B)));
    assert(prebuffered.pop(out));
    assert(out.header.sequenceNumber == 10);
    assert(prebuffered.pop(out));
    assert(out.header.sequenceNumber == 11);
    assert(prebuffered.pop(out));
    assert(out.header.sequenceNumber == 12);

    media::JitterBuffer gapBuffer(8, 1, std::chrono::milliseconds(10));
    assert(gapBuffer.push(makePacket(300, 1)));
    assert(gapBuffer.pop(out));
    assert(out.header.sequenceNumber == 300);
    assert(gapBuffer.push(makePacket(302, 3)));
    assert(!gapBuffer.pop(out));
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    assert(gapBuffer.pop(out));
    assert(out.header.sequenceNumber == 302);

    return 0;
}
