#include "NoiseSuppressor.h"

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

float dbToLinear(float db) {
    return std::pow(10.0F, db / 20.0F);
}

}  // namespace

bool NoiseSuppressor::configure(const Config& config) {
    if (config.sampleRate <= 0 || config.channels <= 0) {
        return false;
    }
    if (config.maxSuppressionDb < 0.0F || config.floorGain <= 0.0F || config.floorGain > 1.0F) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_noiseFloorRms = 0.005F;
    m_configured = true;
    return true;
}

void NoiseSuppressor::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_noiseFloorRms = 0.005F;
}

void NoiseSuppressor::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
}

bool NoiseSuppressor::enabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

bool NoiseSuppressor::frameMatchesConfig(const av::capture::AudioFrame& frame) const {
    return frame.sampleRate == m_config.sampleRate &&
           frame.channels == m_config.channels &&
           !frame.samples.empty() &&
           (frame.samples.size() % static_cast<std::size_t>(m_config.channels)) == 0;
}

bool NoiseSuppressor::processFrame(av::capture::AudioFrame& frame, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    Config config{};
    float noiseFloor = 0.005F;
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
        config = m_config;
        noiseFloor = m_noiseFloorRms;
    }

    const float rms = frameRms(frame.samples);
    if (rms <= 1.0e-7F) {
        return true;
    }

    // Smooth noise floor tracking: adapt quickly in quiet sections, slowly during speech.
    const bool nearNoiseOnly = rms < noiseFloor * 1.8F;
    const float alpha = nearNoiseOnly ? 0.08F : 0.002F;
    noiseFloor = std::clamp((1.0F - alpha) * noiseFloor + alpha * rms, 1.0e-5F, 0.2F);

    const float snr = rms / std::max(1.0e-6F, noiseFloor);
    float suppressionDb = 0.0F;
    if (snr < 1.5F) {
        suppressionDb = -config.maxSuppressionDb;
    } else if (snr < 6.0F) {
        const float t = (snr - 1.5F) / (6.0F - 1.5F);
        suppressionDb = -config.maxSuppressionDb * (1.0F - t);
    }
    const float gain = std::clamp(dbToLinear(suppressionDb), config.floorGain, 1.0F);

    for (float& sample : frame.samples) {
        sample = clampSample(sample * gain);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_noiseFloorRms = noiseFloor;
    }
    return true;
}

}  // namespace av::process
