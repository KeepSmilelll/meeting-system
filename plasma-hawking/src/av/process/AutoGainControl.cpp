#include "AutoGainControl.h"

#include <algorithm>
#include <cmath>

namespace av::process {
namespace {

float clampSample(float value) {
    return std::clamp(value, -1.0F, 1.0F);
}

float frameRms(const std::vector<float>& samples) {
    if (samples.empty()) {
        return 0.0F;
    }
    double energy = 0.0;
    for (float value : samples) {
        energy += static_cast<double>(value) * static_cast<double>(value);
    }
    return static_cast<float>(std::sqrt(energy / static_cast<double>(samples.size())));
}

}  // namespace

bool AutoGainControl::configure(const Config& config) {
    if (config.sampleRate <= 0 || config.channels <= 0) {
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
    m_smoothedGain = 1.0F;
    m_configured = true;
    return true;
}

void AutoGainControl::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_smoothedGain = 1.0F;
}

void AutoGainControl::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
}

bool AutoGainControl::enabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

bool AutoGainControl::frameMatchesConfig(const av::capture::AudioFrame& frame) const {
    return frame.sampleRate == m_config.sampleRate &&
           frame.channels == m_config.channels &&
           !frame.samples.empty() &&
           (frame.samples.size() % static_cast<std::size_t>(m_config.channels)) == 0;
}

bool AutoGainControl::processFrame(av::capture::AudioFrame& frame, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    Config config{};
    float smoothedGain = 1.0F;
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
        config = m_config;
        smoothedGain = m_smoothedGain;
    }

    const float rms = frameRms(frame.samples);
    if (rms < 1.0e-7F) {
        return true;
    }

    const float desiredGain =
        std::clamp(config.targetRms / rms, config.minGain, config.maxGain);
    const float coeff = desiredGain < smoothedGain ? config.attackCoeff : config.releaseCoeff;
    smoothedGain = (1.0F - coeff) * smoothedGain + coeff * desiredGain;
    smoothedGain = std::clamp(smoothedGain, config.minGain, config.maxGain);

    float peak = 0.0F;
    for (const float sample : frame.samples) {
        peak = std::max(peak, std::fabs(sample * smoothedGain));
    }
    if (peak > config.limiterPeak) {
        smoothedGain *= config.limiterPeak / peak;
    }

    for (float& sample : frame.samples) {
        sample = clampSample(sample * smoothedGain);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_smoothedGain = smoothedGain;
    }
    return true;
}

}  // namespace av::process
