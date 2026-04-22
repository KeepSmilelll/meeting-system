#include "AcousticEchoCanceller.h"

#include <algorithm>
#include <cmath>

namespace av::process {
namespace {

float clampSample(float value) {
    return std::clamp(value, -1.0F, 1.0F);
}

float dotProduct(const float* lhs, const float* rhs, std::size_t count) {
    float sum = 0.0F;
    for (std::size_t i = 0; i < count; ++i) {
        sum += lhs[i] * rhs[i];
    }
    return sum;
}

}  // namespace

bool AcousticEchoCanceller::configure(const Config& config) {
    if (config.sampleRate <= 0 || config.channels <= 0 || config.frameSamples <= 0) {
        return false;
    }
    if (config.renderHistoryMs <= 0) {
        return false;
    }
    if (config.suppression < 0.0F || config.suppression > 1.0F) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    const std::size_t samplesPerMs =
        static_cast<std::size_t>(m_config.sampleRate) * static_cast<std::size_t>(m_config.channels) / 1000U;
    m_maxHistorySamples = std::max<std::size_t>(
        static_cast<std::size_t>(m_config.frameSamples) * static_cast<std::size_t>(m_config.channels),
        samplesPerMs * static_cast<std::size_t>(m_config.renderHistoryMs));
    m_renderHistory.clear();
    m_configured = true;
    return true;
}

void AcousticEchoCanceller::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_renderHistory.clear();
}

void AcousticEchoCanceller::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
}

bool AcousticEchoCanceller::enabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

bool AcousticEchoCanceller::frameMatchesConfig(const av::capture::AudioFrame& frame) const {
    return frame.sampleRate == m_config.sampleRate &&
           frame.channels == m_config.channels &&
           !frame.samples.empty() &&
           (frame.samples.size() % static_cast<std::size_t>(m_config.channels)) == 0;
}

void AcousticEchoCanceller::pushRenderFrame(const av::capture::AudioFrame& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_configured || !m_enabled || !frameMatchesConfig(frame)) {
        return;
    }

    m_renderHistory.insert(m_renderHistory.end(), frame.samples.begin(), frame.samples.end());
    if (m_renderHistory.size() > m_maxHistorySamples) {
        const std::size_t overflow = m_renderHistory.size() - m_maxHistorySamples;
        m_renderHistory.erase(m_renderHistory.begin(),
                              m_renderHistory.begin() + static_cast<std::ptrdiff_t>(overflow));
    }
}

bool AcousticEchoCanceller::processCaptureFrame(av::capture::AudioFrame& frame, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    std::vector<float> renderTail;
    Config config{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_configured || !m_enabled) {
            return true;
        }
        if (!frameMatchesConfig(frame)) {
            if (error != nullptr) {
                *error = "aec frame shape mismatch";
            }
            return false;
        }
        if (m_renderHistory.size() < frame.samples.size()) {
            return true;
        }
        config = m_config;
        const std::size_t tailOffset = m_renderHistory.size() - frame.samples.size();
        renderTail.assign(m_renderHistory.begin() + static_cast<std::ptrdiff_t>(tailOffset), m_renderHistory.end());
    }

    const float* capture = frame.samples.data();
    const float* render = renderTail.data();
    const std::size_t count = frame.samples.size();
    const float dotCR = dotProduct(capture, render, count);
    const float dotRR = dotProduct(render, render, count);
    if (dotRR < 1.0e-8F) {
        return true;
    }

    // Single-tap NLMS-like estimate: enough to remove dominant echo energy without blocking the send loop.
    float echoGain = dotCR / dotRR;
    echoGain = std::clamp(echoGain, 0.0F, 2.0F);
    if (echoGain < 0.02F) {
        return true;
    }
    const float suppressGain = echoGain * config.suppression;

    for (std::size_t i = 0; i < count; ++i) {
        frame.samples[i] = clampSample(frame.samples[i] - suppressGain * render[i]);
    }
    return true;
}

}  // namespace av::process
