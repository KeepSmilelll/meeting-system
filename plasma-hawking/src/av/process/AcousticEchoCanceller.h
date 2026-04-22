#pragma once

#include "av/capture/AudioCapture.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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

    AcousticEchoCanceller() = default;

    bool configure(const Config& config);
    void reset();
    void setEnabled(bool enabled);
    bool enabled() const;

    void pushRenderFrame(const av::capture::AudioFrame& frame);
    bool processCaptureFrame(av::capture::AudioFrame& frame, std::string* error = nullptr);

private:
    bool frameMatchesConfig(const av::capture::AudioFrame& frame) const;

    Config m_config{};
    bool m_configured{false};
    bool m_enabled{true};

    mutable std::mutex m_mutex;
    std::vector<float> m_renderHistory;
    std::size_t m_maxHistorySamples{0};
};

}  // namespace av::process
