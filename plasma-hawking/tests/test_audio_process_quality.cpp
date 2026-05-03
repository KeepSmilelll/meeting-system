#include "av/process/AcousticEchoCanceller.h"
#include "av/process/AutoGainControl.h"
#include "av/process/NoiseSuppressor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameSamples = 960;

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

float clampSample(float value) {
    return std::clamp(value, -1.0F, 1.0F);
}

av::capture::AudioFrame makeToneFrame(float frequencyHz,
                                      float amplitude,
                                      int frameIndex,
                                      int sampleRate = kSampleRate,
                                      int channels = kChannels,
                                      int frameSamples = kFrameSamples) {
    av::capture::AudioFrame frame;
    frame.sampleRate = sampleRate;
    frame.channels = channels;
    frame.pts = static_cast<int64_t>(frameIndex) * frameSamples;
    frame.samples.resize(static_cast<std::size_t>(frameSamples) * static_cast<std::size_t>(channels));
    const float phaseBase = 2.0F * 3.14159265358979323846F * static_cast<float>(frameIndex) * 0.13F;
    for (int i = 0; i < frameSamples; ++i) {
        const float phase = 2.0F * 3.14159265358979323846F * frequencyHz *
                            static_cast<float>(i) / static_cast<float>(sampleRate) + phaseBase;
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

void addScaled(std::vector<float>& dst, const std::vector<float>& src, float scale) {
    if (dst.size() != src.size()) {
        return;
    }
    for (std::size_t i = 0; i < dst.size(); ++i) {
        dst[i] = clampSample(dst[i] + src[i] * scale);
    }
}

int runDoubleTalkQualityCheck() {
    av::process::AcousticEchoCanceller aec;
    av::process::NoiseSuppressor ns;
    av::process::AutoGainControl agc;

    av::process::AcousticEchoCanceller::Config aecCfg{};
    aecCfg.sampleRate = kSampleRate;
    aecCfg.channels = kChannels;
    aecCfg.frameSamples = kFrameSamples;
    aecCfg.suppression = 0.85F;
    if (!aec.configure(aecCfg)) {
        std::cerr << "quality aec configure failed" << std::endl;
        return 1;
    }

    av::process::NoiseSuppressor::Config nsCfg{};
    nsCfg.sampleRate = kSampleRate;
    nsCfg.channels = kChannels;
    nsCfg.maxSuppressionDb = 18.0F;
    nsCfg.floorGain = 0.18F;
    if (!ns.configure(nsCfg)) {
        std::cerr << "quality ns configure failed" << std::endl;
        return 1;
    }

    av::process::AutoGainControl::Config agcCfg{};
    agcCfg.sampleRate = kSampleRate;
    agcCfg.channels = kChannels;
    agcCfg.targetRms = 0.12F;
    agcCfg.minGain = 0.25F;
    agcCfg.maxGain = 8.0F;
    if (!agc.configure(agcCfg)) {
        std::cerr << "quality agc configure failed" << std::endl;
        return 1;
    }
    if (std::string(aec.backendName()).find("SpeexDSP") == std::string::npos ||
        std::string(ns.backendName()).find("RNNoise") == std::string::npos ||
        std::string(agc.backendName()).find("SpeexDSP") == std::string::npos) {
        std::cerr << "quality backend mismatch" << std::endl;
        return 1;
    }

    float accumulatedEchoCorrBefore = 0.0F;
    float accumulatedEchoCorrAfter = 0.0F;
    float accumulatedSpeechCorrBefore = 0.0F;
    float accumulatedSpeechCorrAfter = 0.0F;

    for (int frameIndex = 0; frameIndex < 30; ++frameIndex) {
        av::capture::AudioFrame render = makeToneFrame(650.0F, 0.28F, frameIndex);
        av::capture::AudioFrame speech = makeToneFrame(1300.0F, 0.10F, frameIndex);
        av::capture::AudioFrame capture = speech;
        addScaled(capture.samples, render.samples, 0.65F);
        addScaled(capture.samples, makeNoise(capture.samples.size(), 0.01F, 100 + frameIndex), 1.0F);

        const float echoBefore = correlation(capture.samples, render.samples);
        const float speechBefore = correlation(capture.samples, speech.samples);

        aec.pushRenderFrame(render);
        std::string error;
        if (!aec.processCaptureFrame(capture, &error)) {
            std::cerr << "quality aec process failed: " << error << std::endl;
            return 1;
        }
        if (!ns.processFrame(capture, &error)) {
            std::cerr << "quality ns process failed: " << error << std::endl;
            return 1;
        }
        if (!agc.processFrame(capture, &error)) {
            std::cerr << "quality agc process failed: " << error << std::endl;
            return 1;
        }

        const float echoAfter = correlation(capture.samples, render.samples);
        const float speechAfter = std::fabs(correlation(capture.samples, speech.samples));
        accumulatedEchoCorrBefore += echoBefore;
        accumulatedEchoCorrAfter += echoAfter;
        accumulatedSpeechCorrBefore += speechBefore;
        accumulatedSpeechCorrAfter += speechAfter;
    }

    const float avgEchoBefore = accumulatedEchoCorrBefore / 30.0F;
    const float avgEchoAfter = accumulatedEchoCorrAfter / 30.0F;
    const float avgSpeechBefore = accumulatedSpeechCorrBefore / 30.0F;
    const float avgSpeechAfter = accumulatedSpeechCorrAfter / 30.0F;

    if (!(avgEchoAfter < avgEchoBefore * 0.95F)) {
        std::cerr << "echo suppression not enough: before=" << avgEchoBefore
                  << " after=" << avgEchoAfter << std::endl;
        return 1;
    }
    if (!(avgSpeechAfter > 0.20F && avgSpeechAfter > avgSpeechBefore * 0.40F)) {
        std::cerr << "speech preserved too poorly: before=" << avgSpeechBefore
                  << " after=" << avgSpeechAfter << std::endl;
        return 1;
    }
    return 0;
}

int runNoiseOnlySuppressionCheck() {
    av::process::NoiseSuppressor ns;
    av::process::NoiseSuppressor::Config config{};
    config.sampleRate = kSampleRate;
    config.channels = kChannels;
    config.maxSuppressionDb = 24.0F;
    config.floorGain = 0.1F;
    if (!ns.configure(config)) {
        std::cerr << "ns configure failed" << std::endl;
        return 1;
    }
    if (std::string(ns.backendName()).find("RNNoise") == std::string::npos) {
        std::cerr << "ns backend mismatch: " << ns.backendName() << std::endl;
        return 1;
    }

    std::string error;
    av::capture::AudioFrame noiseFrame;
    noiseFrame.sampleRate = kSampleRate;
    noiseFrame.channels = kChannels;
    noiseFrame.samples = makeNoise(static_cast<std::size_t>(kFrameSamples), 0.02F, 42);

    const float before = rms(noiseFrame.samples);
    if (!ns.processFrame(noiseFrame, &error)) {
        std::cerr << "ns process failed: " << error << std::endl;
        return 1;
    }
    const float after = rms(noiseFrame.samples);
    if (!(after < before * 0.85F)) {
        std::cerr << "noise suppression ineffective: before=" << before << " after=" << after << std::endl;
        return 1;
    }
    return 0;
}

int runAgcStabilityCheck() {
    av::process::AutoGainControl agc;
    av::process::AutoGainControl::Config config{};
    config.sampleRate = kSampleRate;
    config.channels = kChannels;
    config.targetRms = 0.12F;
    config.minGain = 0.25F;
    config.maxGain = 8.0F;
    if (!agc.configure(config)) {
        std::cerr << "agc configure failed" << std::endl;
        return 1;
    }
    if (std::string(agc.backendName()).find("SpeexDSP") == std::string::npos) {
        std::cerr << "agc backend mismatch: " << agc.backendName() << std::endl;
        return 1;
    }

    std::string error;
    float firstQuietRms = 0.0F;
    float finalQuietRms = 0.0F;
    for (int i = 0; i < 80; ++i) {
        av::capture::AudioFrame quiet = makeToneFrame(1000.0F, 0.02F, i);
        if (!agc.processFrame(quiet, &error)) {
            std::cerr << "agc process quiet failed: " << error << std::endl;
            return 1;
        }
        const float currentRms = rms(quiet.samples);
        if (i == 0) {
            firstQuietRms = currentRms;
        }
        finalQuietRms = currentRms;
    }
    if (!(finalQuietRms > firstQuietRms * 3.5F)) {
        std::cerr << "agc convergence too weak: first=" << firstQuietRms
                  << " final=" << finalQuietRms << std::endl;
        return 1;
    }
    if (!(finalQuietRms > 0.08F && finalQuietRms < 0.13F)) {
        std::cerr << "agc steady rms out of range: " << finalQuietRms << std::endl;
        return 1;
    }

    av::capture::AudioFrame loud = makeToneFrame(400.0F, 0.90F, 2);
    if (!agc.processFrame(loud, &error)) {
        std::cerr << "agc process loud failed: " << error << std::endl;
        return 1;
    }
    const float peak = *std::max_element(loud.samples.begin(), loud.samples.end(),
                                         [](float lhs, float rhs) { return std::fabs(lhs) < std::fabs(rhs); });
    if (!(std::fabs(peak) <= 0.97F)) {
        std::cerr << "agc limiter failed, peak=" << peak << std::endl;
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    if (runDoubleTalkQualityCheck() != 0) {
        return 1;
    }
    if (runNoiseOnlySuppressionCheck() != 0) {
        return 1;
    }
    if (runAgcStabilityCheck() != 0) {
        return 1;
    }
    return 0;
}
