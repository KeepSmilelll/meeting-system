#pragma once

#include "common/RingBuffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace av::capture {

struct ScreenFrame {
    int width{0};
    int height{0};
    int64_t pts{0};
    std::vector<uint8_t> bgra;
};

struct ScreenCaptureConfig {
    int targetWidth{1280};
    int targetHeight{720};
    int frameRate{5};
    std::size_t ringCapacity{3};
};

class ScreenCapture {
public:
    explicit ScreenCapture(ScreenCaptureConfig config = {});
    ~ScreenCapture();

    bool start();
    void stop();
    bool isRunning() const;

    bool popFrameForEncode(ScreenFrame& outFrame, std::chrono::milliseconds timeout);

private:
    struct Impl;
    friend struct Impl;

    ScreenCaptureConfig m_config;
    std::atomic<bool> m_running{false};
    std::unique_ptr<Impl> m_impl;
    common::RingBuffer<ScreenFrame> m_ringBuffer;
};

}  // namespace av::capture
