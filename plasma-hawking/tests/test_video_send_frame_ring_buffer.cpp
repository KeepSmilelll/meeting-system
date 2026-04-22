#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

#include "av/session/VideoSendFrameRingBuffer.h"

namespace {

av::session::VideoSendCapturedFrame makeScreenFrame(int64_t pts) {
    av::session::VideoSendCapturedFrame frame;
    frame.source = av::session::VideoSendSource::Screen;
    frame.inputFrame.screenFrame.width = 16;
    frame.inputFrame.screenFrame.height = 16;
    frame.inputFrame.screenFrame.pts = pts;
    frame.inputFrame.screenFrame.bgra.assign(16U * 16U * 4U, static_cast<uint8_t>(pts & 0xFF));
    return frame;
}

void testDropOldestWhenFull() {
    av::session::VideoSendFrameRingBuffer buffer(2);
    assert(buffer.push(makeScreenFrame(1)));
    assert(buffer.push(makeScreenFrame(2)));
    assert(buffer.push(makeScreenFrame(3)));  // drop oldest(1)

    av::session::VideoSendCapturedFrame out;
    assert(buffer.popWait(out, std::chrono::milliseconds(10)));
    assert(out.inputFrame.screenFrame.pts == 2);
    assert(buffer.popWait(out, std::chrono::milliseconds(10)));
    assert(out.inputFrame.screenFrame.pts == 3);
}

void testCloseWakeup() {
    av::session::VideoSendFrameRingBuffer buffer(4);
    std::atomic<bool> popReturned{false};
    std::atomic<bool> popValue{true};
    std::thread waiter([&]() {
        av::session::VideoSendCapturedFrame out;
        popValue.store(buffer.popWait(out, std::chrono::milliseconds(3000)), std::memory_order_release);
        popReturned.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    buffer.close();
    waiter.join();
    assert(popReturned.load(std::memory_order_acquire));
    assert(!popValue.load(std::memory_order_acquire));
}

void testConcurrentPushPop() {
    av::session::VideoSendFrameRingBuffer buffer(8);
    std::atomic<bool> producerDone{false};
    std::atomic<int> receivedCount{0};
    std::atomic<int64_t> lastPts{0};

    std::thread producer([&]() {
        for (int i = 1; i <= 200; ++i) {
            (void)buffer.push(makeScreenFrame(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        producerDone.store(true, std::memory_order_release);
        buffer.close();
    });

    std::thread consumer([&]() {
        av::session::VideoSendCapturedFrame out;
        while (buffer.popWait(out, std::chrono::milliseconds(200))) {
            const int64_t pts = out.inputFrame.screenFrame.pts;
            assert(pts >= lastPts.load(std::memory_order_acquire));
            lastPts.store(pts, std::memory_order_release);
            receivedCount.fetch_add(1, std::memory_order_acq_rel);
        }
    });

    producer.join();
    consumer.join();
    assert(producerDone.load(std::memory_order_acquire));
    assert(receivedCount.load(std::memory_order_acquire) > 0);
    assert(lastPts.load(std::memory_order_acquire) > 0);
}

}  // namespace

int main() {
    testDropOldestWhenFull();
    testCloseWakeup();
    testConcurrentPushPop();
    return 0;
}

