#pragma once

#include "av/capture/AudioCapture.h"

#include <mutex>
#include <string>

namespace av::process {

class NoiseSuppressor {
public:
    struct Config {
        int sampleRate{48000};
        int channels{1};
        float maxSuppressionDb{18.0F};
        float floorGain{0.18F};
    };

    NoiseSuppressor() = default;

    bool configure(const Config& config);
    void reset();
    void setEnabled(bool enabled);
    bool enabled() const;

    bool processFrame(av::capture::AudioFrame& frame, std::string* error = nullptr);

private:
    bool frameMatchesConfig(const av::capture::AudioFrame& frame) const;

    Config m_config{};
    bool m_configured{false};
    bool m_enabled{true};

    mutable std::mutex m_mutex;
    float m_noiseFloorRms{0.005F};
};

}  // namespace av::process
