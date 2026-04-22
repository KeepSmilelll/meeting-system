#include "av/process/AcousticEchoCanceller.h"
#include "av/process/AutoGainControl.h"
#include "av/process/NoiseSuppressor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

namespace {

float rms(const std::vector<float>& samples) {
    if (samples.empty()) {
        return 0.0F;
    }
    double energy = 0.0;
    for (float value : samples) {
        energy += static_cast<double>(value) * static_cast<double>(value);
    }
    return static_cast<float>(std::sqrt(energy / static_cast<double>(samples.size())));
}

float correlation(const std::vector<float>& lhs, const std::vector<float>& rhs) {
    if (lhs.size() != rhs.size() || lhs.empty()) {
        return 0.0F;
    }
    double dot = 0.0;
    double lhsNorm = 0.0;
    double rhsNorm = 0.0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        dot += static_cast<double>(lhs[i]) * static_cast<double>(rhs[i]);
        lhsNorm += static_cast<double>(lhs[i]) * static_cast<double>(lhs[i]);
        rhsNorm += static_cast<double>(rhs[i]) * static_cast<double>(rhs[i]);
    }
    if (lhsNorm <= 1.0e-12 || rhsNorm <= 1.0e-12) {
        return 0.0F;
    }
    return static_cast<float>(dot / std::sqrt(lhsNorm * rhsNorm));
}

av::capture::AudioFrame makeSineFrame(float frequencyHz,
                                      float amplitude,
                                      int sampleRate,
                                      int channels,
                                      int frameSamples,
                                      float phaseOffset = 0.0F) {
    av::capture::AudioFrame frame;
    frame.sampleRate = sampleRate;
    frame.channels = channels;
    frame.samples.resize(static_cast<std::size_t>(frameSamples) * static_cast<std::size_t>(channels));
    for (int i = 0; i < frameSamples; ++i) {
        const float phase = 2.0F * 3.14159265358979323846F * frequencyHz *
                            static_cast<float>(i) / static_cast<float>(sampleRate) + phaseOffset;
        const float sample = amplitude * std::sin(phase);
        for (int ch = 0; ch < channels; ++ch) {
            frame.samples[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) +
                          static_cast<std::size_t>(ch)] = sample;
        }
    }
    return frame;
}

int runAgcCheck() {
    constexpr int kSampleRate = 48000;
    constexpr int kChannels = 1;
    constexpr int kFrameSamples = 960;

    av::process::AutoGainControl agc;
    av::process::AutoGainControl::Config config{};
    config.sampleRate = kSampleRate;
    config.channels = kChannels;
    config.targetRms = 0.15F;
    if (!agc.configure(config)) {
        std::cerr << "agc configure failed" << std::endl;
        return 1;
    }

    av::capture::AudioFrame frame = makeSineFrame(1000.0F, 0.02F, kSampleRate, kChannels, kFrameSamples);
    const float before = rms(frame.samples);
    std::string error;
    if (!agc.processFrame(frame, &error)) {
        std::cerr << "agc process failed: " << error << std::endl;
        return 1;
    }
    const float after = rms(frame.samples);
    if (!(after > before * 1.5F)) {
        std::cerr << "agc gain ineffective: before=" << before << " after=" << after << std::endl;
        return 1;
    }
    return 0;
}

int runNoiseSuppressorCheck() {
    constexpr int kSampleRate = 48000;
    constexpr int kChannels = 1;
    constexpr int kFrameSamples = 960;

    av::process::NoiseSuppressor ns;
    av::process::NoiseSuppressor::Config config{};
    config.sampleRate = kSampleRate;
    config.channels = kChannels;
    config.maxSuppressionDb = 24.0F;
    if (!ns.configure(config)) {
        std::cerr << "ns configure failed" << std::endl;
        return 1;
    }

    std::string error;
    for (int i = 0; i < 8; ++i) {
        av::capture::AudioFrame baseline = makeSineFrame(180.0F, 0.01F, kSampleRate, kChannels, kFrameSamples, static_cast<float>(i));
        if (!ns.processFrame(baseline, &error)) {
            std::cerr << "ns baseline process failed: " << error << std::endl;
            return 1;
        }
    }

    av::capture::AudioFrame noisy = makeSineFrame(180.0F, 0.01F, kSampleRate, kChannels, kFrameSamples);
    const float before = rms(noisy.samples);
    if (!ns.processFrame(noisy, &error)) {
        std::cerr << "ns process failed: " << error << std::endl;
        return 1;
    }
    const float after = rms(noisy.samples);
    if (!(after < before)) {
        std::cerr << "ns suppression ineffective: before=" << before << " after=" << after << std::endl;
        return 1;
    }
    return 0;
}

int runAecCheck() {
    constexpr int kSampleRate = 48000;
    constexpr int kChannels = 1;
    constexpr int kFrameSamples = 960;

    av::process::AcousticEchoCanceller aec;
    av::process::AcousticEchoCanceller::Config config{};
    config.sampleRate = kSampleRate;
    config.channels = kChannels;
    config.frameSamples = kFrameSamples;
    config.suppression = 0.9F;
    if (!aec.configure(config)) {
        std::cerr << "aec configure failed" << std::endl;
        return 1;
    }

    av::capture::AudioFrame render = makeSineFrame(700.0F, 0.30F, kSampleRate, kChannels, kFrameSamples);
    av::capture::AudioFrame voice = makeSineFrame(1400.0F, 0.10F, kSampleRate, kChannels, kFrameSamples);
    av::capture::AudioFrame capture = render;
    for (std::size_t i = 0; i < capture.samples.size(); ++i) {
        capture.samples[i] = std::clamp(capture.samples[i] * 0.65F + voice.samples[i], -1.0F, 1.0F);
    }

    aec.pushRenderFrame(render);
    const float corrBefore = correlation(capture.samples, render.samples);
    std::string error;
    if (!aec.processCaptureFrame(capture, &error)) {
        std::cerr << "aec process failed: " << error << std::endl;
        return 1;
    }
    const float corrAfter = correlation(capture.samples, render.samples);
    if (!(corrAfter < corrBefore)) {
        std::cerr << "aec suppression ineffective: before=" << corrBefore << " after=" << corrAfter << std::endl;
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    if (runAgcCheck() != 0) {
        return 1;
    }
    if (runNoiseSuppressorCheck() != 0) {
        return 1;
    }
    if (runAecCheck() != 0) {
        return 1;
    }
    return 0;
}
