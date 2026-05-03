#include "AutoGainControl.h"

#include <speex/speex_preprocess.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace av::process {
namespace {

spx_int16_t floatToInt16(float value) {
    const float clamped = std::clamp(value, -1.0F, 1.0F);
    return static_cast<spx_int16_t>(std::lrint(clamped * 32767.0F));
}

float int16ToFloat(spx_int16_t value) {
    return std::clamp(static_cast<float>(value) / 32768.0F, -1.0F, 1.0F);
}

}  // namespace

struct AutoGainControl::Impl {
    ~Impl() {
        for (SpeexPreprocessState* state : states) {
            speex_preprocess_state_destroy(state);
        }
    }

    std::vector<SpeexPreprocessState*> states;
    std::vector<spx_int16_t> channel;
};

AutoGainControl::AutoGainControl() = default;
AutoGainControl::~AutoGainControl() = default;

bool AutoGainControl::configure(const Config& config) {
    if (config.sampleRate <= 0 || config.channels <= 0 || config.frameSamples <= 0) {
        return false;
    }
    if (config.targetRms <= 0.0F || config.minGain <= 0.0F || config.maxGain < config.minGain) {
        return false;
    }
    if (config.attackCoeff < 0.0F || config.attackCoeff > 1.0F ||
        config.releaseCoeff < 0.0F || config.releaseCoeff > 1.0F) {
        return false;
    }
    if (config.limiterPeak <= 0.0F || config.limiterPeak > 1.0F) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    return createBackendLocked();
}

void AutoGainControl::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    (void)createBackendLocked();
}

void AutoGainControl::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
}

bool AutoGainControl::enabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

const char* AutoGainControl::backendName() const {
    return "SpeexDSP AGC";
}

bool AutoGainControl::frameMatchesConfig(const av::capture::AudioFrame& frame) const {
    const auto expectedSamples =
        static_cast<std::size_t>(m_config.frameSamples) * static_cast<std::size_t>(m_config.channels);
    return frame.sampleRate == m_config.sampleRate &&
           frame.channels == m_config.channels &&
           !frame.samples.empty() &&
           expectedSamples > 0 &&
           (frame.samples.size() % expectedSamples) == 0;
}

bool AutoGainControl::createBackendLocked() {
    auto impl = std::make_unique<Impl>();
    impl->states.reserve(static_cast<std::size_t>(m_config.channels));
    for (int channel = 0; channel < m_config.channels; ++channel) {
        SpeexPreprocessState* state = speex_preprocess_state_init(m_config.frameSamples, m_config.sampleRate);
        if (state == nullptr) {
            m_impl.reset();
            m_configured = false;
            return false;
        }
        int disabled = 0;
        int enabled = 1;
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DENOISE, &disabled);
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC, &enabled);

        float agcLevel = std::clamp(m_config.targetRms * 32767.0F, 512.0F, 30000.0F);
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agcLevel);
        spx_int32_t agcTarget = static_cast<spx_int32_t>(std::lround(agcLevel));
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC_TARGET, &agcTarget);

        int incrementDb = std::max(1, static_cast<int>(std::lround(120.0F * m_config.releaseCoeff)));
        int decrementDb = std::max(1, static_cast<int>(std::lround(120.0F * m_config.attackCoeff)));
        int maxGainDb = std::max(0, static_cast<int>(std::lround(20.0F * std::log10(m_config.maxGain))));
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC_INCREMENT, &incrementDb);
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC_DECREMENT, &decrementDb);
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &maxGainDb);

        impl->states.push_back(state);
    }
    impl->channel.resize(static_cast<std::size_t>(m_config.frameSamples));
    m_impl = std::move(impl);
    m_configured = true;
    return true;
}

bool AutoGainControl::processFrame(av::capture::AudioFrame& frame, std::string* error) {
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
                *error = "agc frame shape mismatch";
            }
            return false;
        }
        if (m_impl == nullptr) {
            if (error != nullptr) {
                *error = "agc backend unavailable";
            }
            return false;
        }

        const std::size_t channels = static_cast<std::size_t>(m_config.channels);
        const std::size_t frameSamples = static_cast<std::size_t>(m_config.frameSamples);
        const std::size_t chunkSamples = frameSamples * channels;
        for (std::size_t offset = 0; offset < frame.samples.size(); offset += chunkSamples) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                for (std::size_t i = 0; i < frameSamples; ++i) {
                    const std::size_t srcIndex = offset + i * channels + channel;
                    m_impl->channel[i] = floatToInt16(frame.samples[srcIndex]);
                }
                speex_preprocess_run(m_impl->states[channel], m_impl->channel.data());
                for (std::size_t i = 0; i < frameSamples; ++i) {
                    const std::size_t dstIndex = offset + i * channels + channel;
                    frame.samples[dstIndex] = int16ToFloat(m_impl->channel[i]);
                }
            }
        }
    }
    return true;
}

}  // namespace av::process
