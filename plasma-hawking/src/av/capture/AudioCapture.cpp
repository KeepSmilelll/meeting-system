#include "AudioCapture.h"

namespace av::capture {

AudioCapture::AudioCapture(std::size_t ringCapacity)
    : m_ringBuffer(ringCapacity) {}

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        return false;
    }
    return true;
}

void AudioCapture::stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    m_ringBuffer.close();
}

bool AudioCapture::isRunning() const {
    return m_running.load(std::memory_order_relaxed);
}

bool AudioCapture::pushCapturedFrame(AudioFrame frame) {
    if (!isRunning()) {
        return false;
    }
    return m_ringBuffer.push(std::move(frame));
}

bool AudioCapture::popFrameForEncode(AudioFrame& outFrame, std::chrono::milliseconds timeout) {
    if (!isRunning()) {
        return false;
    }
    return m_ringBuffer.popWait(outFrame, timeout);
}

}  // namespace av::capture
