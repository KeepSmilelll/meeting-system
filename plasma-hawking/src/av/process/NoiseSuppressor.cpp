#include "NoiseSuppressor.h"

#include <rnnoise.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace av::process {
namespace {

float clampSample(float value) {
    return std::clamp(value, -1.0F, 1.0F);
}

}  // namespace

struct NoiseSuppressor::Impl {
    ~Impl() {
        for (DenoiseState* state : states) {
            rnnoise_destroy(state);
        }
    }

    std::vector<DenoiseState*> states;
    std::vector<float> channelInput;
    std::vector<float> channelOutput;
    int rnnoiseFrameSamples{0};
};

NoiseSuppressor::NoiseSuppressor() = default;
NoiseSuppressor::~NoiseSuppressor() = default;

bool NoiseSuppressor::configure(const Config& config) {
    if (config.sampleRate != 48000 || config.channels <= 0) {
        return false;
    }
    if (config.maxSuppressionDb < 0.0F || config.floorGain <= 0.0F || config.floorGain > 1.0F) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    return createBackendLocked();
}

void NoiseSuppressor::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    (void)createBackendLocked();
}

void NoiseSuppressor::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
}

bool NoiseSuppressor::enabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

const char* NoiseSuppressor::backendName() const {
    return "RNNoise";
}

bool NoiseSuppressor::frameMatchesConfig(const av::capture::AudioFrame& frame) const {
    const int rnnoiseFrameSamples = m_impl != nullptr ? m_impl->rnnoiseFrameSamples : 0;
    const auto chunkSamples =
        static_cast<std::size_t>(rnnoiseFrameSamples) * static_cast<std::size_t>(m_config.channels);
    return frame.sampleRate == m_config.sampleRate &&
           frame.channels == m_config.channels &&
           !frame.samples.empty() &&
           chunkSamples > 0 &&
           (frame.samples.size() % chunkSamples) == 0;
}

bool NoiseSuppressor::createBackendLocked() {
    auto impl = std::make_unique<Impl>();
    impl->rnnoiseFrameSamples = rnnoise_get_frame_size();
    if (impl->rnnoiseFrameSamples <= 0) {
        m_impl.reset();
        m_configured = false;
        return false;
    }
    impl->states.reserve(static_cast<std::size_t>(m_config.channels));
    for (int channel = 0; channel < m_config.channels; ++channel) {
        DenoiseState* state = rnnoise_create(nullptr);
        if (state == nullptr) {
            m_impl.reset();
            m_configured = false;
            return false;
        }
        impl->states.push_back(state);
    }
    impl->channelInput.resize(static_cast<std::size_t>(impl->rnnoiseFrameSamples));
    impl->channelOutput.resize(static_cast<std::size_t>(impl->rnnoiseFrameSamples));
    m_impl = std::move(impl);
    m_configured = true;
    return true;
}

bool NoiseSuppressor::processFrame(av::capture::AudioFrame& frame, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_configured || !m_enabled) {
            return true;
        }
        if (!frameMatchesConfig(frame)) {
            if (error != nullptr) {
                *error = "ns frame shape mismatch";
            }
            return false;
        }
        if (m_impl == nullptr) {
            if (error != nullptr) {
                *error = "ns backend unavailable";
            }
            return false;
        }

        const int frameSamples = m_impl->rnnoiseFrameSamples;
        const std::size_t channels = static_cast<std::size_t>(m_config.channels);
        const std::size_t chunkSamples = static_cast<std::size_t>(frameSamples) * channels;
        for (std::size_t offset = 0; offset < frame.samples.size(); offset += chunkSamples) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                for (int i = 0; i < frameSamples; ++i) {
                    const std::size_t srcIndex = offset + static_cast<std::size_t>(i) * channels + channel;
                    m_impl->channelInput[static_cast<std::size_t>(i)] = frame.samples[srcIndex] * 32768.0F;
                }
                rnnoise_process_frame(m_impl->states[channel],
                                      m_impl->channelOutput.data(),
                                      m_impl->channelInput.data());
                for (int i = 0; i < frameSamples; ++i) {
                    const std::size_t dstIndex = offset + static_cast<std::size_t>(i) * channels + channel;
                    frame.samples[dstIndex] =
                        clampSample(m_impl->channelOutput[static_cast<std::size_t>(i)] / 32768.0F);
                }
            }
        }
    }
    return true;
}

}  // namespace av::process
