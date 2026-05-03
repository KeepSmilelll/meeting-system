#pragma once

#include "av/capture/AudioCapture.h"

#include <memory>
#include <mutex>
#include <string>

namespace av::process {

class AutoGainControl {
public:
    struct Config {
        int sampleRate{48000};
        int channels{1};
        int frameSamples{960};
        float targetRms{0.12F};
        float minGain{0.25F};
        float maxGain{8.0F};
        float attackCoeff{0.35F};
        float releaseCoeff{0.08F};
        float limiterPeak{0.95F};
    };

    AutoGainControl();
    ~AutoGainControl();

    bool configure(const Config& config);
    void reset();
    void setEnabled(bool enabled);
    bool enabled() const;
    const char* backendName() const;

    bool processFrame(av::capture::AudioFrame& frame, std::string* error = nullptr);

private:
    bool frameMatchesConfig(const av::capture::AudioFrame& frame) const;
    bool createBackendLocked();

    Config m_config{};
    bool m_configured{false};
    bool m_enabled{true};

    mutable std::mutex m_mutex;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace av::process
