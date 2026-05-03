#include "AcousticEchoCanceller.h"

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <algorithm>
#include <cmath>
#include <deque>
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

struct AcousticEchoCanceller::Impl {
    ~Impl() {
        if (preprocess != nullptr) {
            speex_preprocess_state_destroy(preprocess);
        }
        if (echo != nullptr) {
            speex_echo_state_destroy(echo);
        }
    }

    SpeexEchoState* echo{nullptr};
    SpeexPreprocessState* preprocess{nullptr};
    std::deque<std::vector<spx_int16_t>> renderFifo;
    std::vector<spx_int16_t> render;
    std::vector<spx_int16_t> capture;
    std::vector<spx_int16_t> output;
    std::vector<spx_int16_t> silence;
    std::size_t maxQueuedRenderFrames{0};
};

AcousticEchoCanceller::AcousticEchoCanceller() = default;
AcousticEchoCanceller::~AcousticEchoCanceller() = default;

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
    return createBackendLocked();
}

void AcousticEchoCanceller::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl != nullptr && m_impl->echo != nullptr) {
        speex_echo_state_reset(m_impl->echo);
        m_impl->renderFifo.clear();
    }
}

void AcousticEchoCanceller::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
}

bool AcousticEchoCanceller::enabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

const char* AcousticEchoCanceller::backendName() const {
    return "SpeexDSP AEC";
}

bool AcousticEchoCanceller::frameMatchesConfig(const av::capture::AudioFrame& frame) const {
    const auto expectedSamples =
        static_cast<std::size_t>(m_config.frameSamples) * static_cast<std::size_t>(m_config.channels);
    return frame.sampleRate == m_config.sampleRate &&
           frame.channels == m_config.channels &&
           !frame.samples.empty() &&
           expectedSamples > 0 &&
           (frame.samples.size() % expectedSamples) == 0;
}

bool AcousticEchoCanceller::createBackendLocked() {
    auto impl = std::make_unique<Impl>();
    const int filterLength = std::max(m_config.frameSamples,
                                      m_config.sampleRate * m_config.renderHistoryMs / 1000);
    impl->echo = speex_echo_state_init_mc(m_config.frameSamples,
                                          filterLength,
                                          m_config.channels,
                                          m_config.channels);
    if (impl->echo == nullptr) {
        m_impl.reset();
        m_configured = false;
        return false;
    }

    speex_echo_ctl(impl->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &m_config.sampleRate);
    impl->preprocess = speex_preprocess_state_init(m_config.frameSamples, m_config.sampleRate);
    if (impl->preprocess != nullptr) {
        int enabled = 0;
        speex_preprocess_ctl(impl->preprocess, SPEEX_PREPROCESS_SET_DENOISE, &enabled);
        speex_preprocess_ctl(impl->preprocess, SPEEX_PREPROCESS_SET_AGC, &enabled);
        speex_preprocess_ctl(impl->preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, impl->echo);
        const int echoSuppress = -static_cast<int>(std::lround(40.0F * m_config.suppression));
        const int echoSuppressActive = -static_cast<int>(std::lround(15.0F * m_config.suppression));
        speex_preprocess_ctl(impl->preprocess, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, const_cast<int*>(&echoSuppress));
        speex_preprocess_ctl(impl->preprocess,
                             SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,
                             const_cast<int*>(&echoSuppressActive));
    }

    const std::size_t frameSamples =
        static_cast<std::size_t>(m_config.frameSamples) * static_cast<std::size_t>(m_config.channels);
    impl->render.resize(frameSamples);
    impl->capture.resize(frameSamples);
    impl->output.resize(frameSamples);
    impl->silence.assign(frameSamples, 0);
    const int framesPerHistory =
        std::max(1, (m_config.sampleRate * m_config.renderHistoryMs) / (1000 * m_config.frameSamples));
    impl->maxQueuedRenderFrames = static_cast<std::size_t>(std::max(2, framesPerHistory));
    m_impl = std::move(impl);
    m_configured = true;
    return true;
}

void AcousticEchoCanceller::pushRenderFrame(const av::capture::AudioFrame& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_configured || !m_enabled || m_impl == nullptr || !frameMatchesConfig(frame)) {
        return;
    }

    const std::size_t chunkSamples =
        static_cast<std::size_t>(m_config.frameSamples) * static_cast<std::size_t>(m_config.channels);
    for (std::size_t offset = 0; offset < frame.samples.size(); offset += chunkSamples) {
        for (std::size_t i = 0; i < chunkSamples; ++i) {
            m_impl->render[i] = floatToInt16(frame.samples[offset + i]);
        }
        m_impl->renderFifo.push_back(m_impl->render);
        while (m_impl->renderFifo.size() > m_impl->maxQueuedRenderFrames) {
            m_impl->renderFifo.pop_front();
        }
    }
}

bool AcousticEchoCanceller::processCaptureFrame(av::capture::AudioFrame& frame, std::string* error) {
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
                *error = "aec frame shape mismatch";
            }
            return false;
        }
        if (m_impl == nullptr) {
            if (error != nullptr) {
                *error = "aec backend unavailable";
            }
            return false;
        }

        const std::size_t chunkSamples =
            static_cast<std::size_t>(m_config.frameSamples) * static_cast<std::size_t>(m_config.channels);
        for (std::size_t offset = 0; offset < frame.samples.size(); offset += chunkSamples) {
            for (std::size_t i = 0; i < chunkSamples; ++i) {
                m_impl->capture[i] = floatToInt16(frame.samples[offset + i]);
            }
            const spx_int16_t* render = m_impl->silence.data();
            if (!m_impl->renderFifo.empty()) {
                m_impl->render = std::move(m_impl->renderFifo.front());
                m_impl->renderFifo.pop_front();
                render = m_impl->render.data();
            }
            speex_echo_cancellation(m_impl->echo, m_impl->capture.data(), render, m_impl->output.data());
            if (m_impl->preprocess != nullptr && m_config.channels == 1) {
                speex_preprocess_run(m_impl->preprocess, m_impl->output.data());
            }
            for (std::size_t i = 0; i < chunkSamples; ++i) {
                frame.samples[offset + i] = int16ToFloat(m_impl->output[i]);
            }
        }
    }
    return true;
}

}  // namespace av::process
