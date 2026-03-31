#pragma once

#include "common/RingBuffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace av::capture {

struct AudioFrame {
    int sampleRate{48000};
    int channels{1};
    int64_t pts{0};
    std::vector<float> samples;
};

class AudioCapture {
public:
    explicit AudioCapture(std::size_t ringCapacity = 64);
    ~AudioCapture();

    bool start();
    void stop();
    bool isRunning() const;

    // Capture-thread entry: push one captured PCM frame.
    bool pushCapturedFrame(AudioFrame frame);

    // Encode-thread entry: pop one PCM frame with timeout.
    bool popFrameForEncode(AudioFrame& outFrame, std::chrono::milliseconds timeout);

private:
    struct Impl;
    friend struct Impl;

    std::atomic<bool> m_running{false};
    std::unique_ptr<Impl> m_impl;
    common::RingBuffer<AudioFrame> m_ringBuffer;
};

}  // namespace av::capture
