#include "av/process/AcousticEchoCanceller.h"
#include "av/process/AutoGainControl.h"
#include "av/process/NoiseSuppressor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
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

std::vector<float> makeNoise(std::size_t count, float amplitude, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    std::vector<float> noise(count);
    for (float& sample : noise) {
        sample = dist(rng);
    }
    return noise;
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
    if (std::string(agc.backendName()).find("SpeexDSP") == std::string::npos) {
        std::cerr << "agc backend mismatch: " << agc.backendName() << std::endl;
        return 1;
    }

    const float before = rms(makeSineFrame(1000.0F, 0.02F, kSampleRate, kChannels, kFrameSamples).samples);
    std::string error;
    float after = 0.0F;
    for (int i = 0; i < 80; ++i) {
        av::capture::AudioFrame frame =
            makeSineFrame(1000.0F, 0.02F, kSampleRate, kChannels, kFrameSamples, static_cast<float>(i));
        if (!agc.processFrame(frame, &error)) {
            std::cerr << "agc process failed: " << error << std::endl;
            return 1;
        }
        after = rms(frame.samples);
    }
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
    if (std::string(ns.backendName()).find("RNNoise") == std::string::npos) {
        std::cerr << "ns backend mismatch: " << ns.backendName() << std::endl;
        return 1;
    }

    std::string error;
    for (int i = 0; i < 8; ++i) {
        av::capture::AudioFrame baseline;
        baseline.sampleRate = kSampleRate;
        baseline.channels = kChannels;
        baseline.samples = makeNoise(static_cast<std::size_t>(kFrameSamples), 0.02F, 100U + static_cast<std::uint32_t>(i));
        if (!ns.processFrame(baseline, &error)) {
            std::cerr << "ns baseline process failed: " << error << std::endl;
            return 1;
        }
    }

    av::capture::AudioFrame noisy;
    noisy.sampleRate = kSampleRate;
    noisy.channels = kChannels;
    noisy.samples = makeNoise(static_cast<std::size_t>(kFrameSamples), 0.02F, 42U);
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
    if (std::string(aec.backendName()).find("SpeexDSP") == std::string::npos) {
        std::cerr << "aec backend mismatch: " << aec.backendName() << std::endl;
        return 1;
    }

    std::string error;
    float corrBefore = 0.0F;
    float corrAfter = 0.0F;
    int measured = 0;
    for (int i = 0; i < 60; ++i) {
        av::capture::AudioFrame render =
            makeSineFrame(700.0F, 0.30F, kSampleRate, kChannels, kFrameSamples, static_cast<float>(i) * 0.1F);
        av::capture::AudioFrame voice =
            makeSineFrame(1400.0F, 0.10F, kSampleRate, kChannels, kFrameSamples, static_cast<float>(i) * 0.2F);
        av::capture::AudioFrame capture = render;
        for (std::size_t sample = 0; sample < capture.samples.size(); ++sample) {
            capture.samples[sample] = std::clamp(capture.samples[sample] * 0.65F + voice.samples[sample], -1.0F, 1.0F);
        }

        const float before = std::fabs(correlation(capture.samples, render.samples));
        aec.pushRenderFrame(render);
        if (!aec.processCaptureFrame(capture, &error)) {
            std::cerr << "aec process failed: " << error << std::endl;
            return 1;
        }
        if (i >= 20) {
            corrBefore += before;
            corrAfter += std::fabs(correlation(capture.samples, render.samples));
            ++measured;
        }
    }
    corrBefore /= static_cast<float>(measured);
    corrAfter /= static_cast<float>(measured);
    if (!(corrAfter < corrBefore * 0.95F)) {
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
