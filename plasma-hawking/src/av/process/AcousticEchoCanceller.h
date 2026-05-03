#pragma once

#include "av/capture/AudioCapture.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace av::process {

class AcousticEchoCanceller {
public:
    struct Config {
        int sampleRate{48000};
        int channels{1};
        int frameSamples{960};
        int renderHistoryMs{500};
        float suppression{0.85F};
    };

    AcousticEchoCanceller();
    ~AcousticEchoCanceller();

    bool configure(const Config& config);
    void reset();
    void setEnabled(bool enabled);
    bool enabled() const;
    const char* backendName() const;

    void pushRenderFrame(const av::capture::AudioFrame& frame);
    bool processCaptureFrame(av::capture::AudioFrame& frame, std::string* error = nullptr);

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
