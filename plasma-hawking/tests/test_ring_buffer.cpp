#include <cassert>
#include <chrono>
#include <thread>

#include "common/RingBuffer.h"

int main() {
    common::RingBuffer<int> buffer(2);
    assert(buffer.capacity() == 2);
    assert(buffer.size() == 0);

    assert(buffer.push(10));
    assert(buffer.push(20));
    assert(!buffer.push(30));
    assert(buffer.size() == 2);

    int out = 0;
    assert(buffer.pop(out));
    assert(out == 10);
    assert(buffer.pop(out));
    assert(out == 20);
    assert(!buffer.pop(out));

    std::thread producer([&buffer]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        (void)buffer.push(42);
    });

    assert(buffer.popWait(out, std::chrono::milliseconds(200)));
    assert(out == 42);
    producer.join();

    buffer.close();
    assert(buffer.closed());
    assert(!buffer.push(99));
    assert(!buffer.popWait(out, std::chrono::milliseconds(20)));

    return 0;
}
