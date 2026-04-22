#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace av::sync {

class AVSync {
public:
    void reset(int64_t pts = -1, int sampleRate = 48000, int frameSamples = 0);
    void update(int64_t pts, int sampleRate, int frameSamples);

    int64_t audioPts() const;
    int64_t audioTimeMs() const;
    int sampleRate() const;
    int frameSamples() const;
    bool hasAudioPts() const;

    static int64_t audioPtsToTimeMs(int64_t pts, int sampleRate);
    static int64_t videoPts90kToTimeMs(int64_t pts90k);
    static int64_t videoAudioSkewMs(int64_t videoPts90k, int64_t audioPts, int audioSampleRate);
    static bool shouldDropVideoFrameByAudioClock(int64_t videoPts90k,
                                                 int64_t audioPts,
                                                 int audioSampleRate,
                                                 int maxLeadMs = 40,
                                                 int maxLagMs = 40);
    static int suggestVideoRenderDelayMsByAudioClock(int64_t videoPts90k,
                                                     int64_t audioPts,
                                                     int audioSampleRate,
                                                     int maxDelayMs = 250);

private:
    std::atomic<int64_t> m_audioPts{-1};
    std::atomic<int> m_sampleRate{48000};
    std::atomic<int> m_frameSamples{0};
};

}  // namespace av::sync

inline void av::sync::AVSync::reset(int64_t pts, int sampleRate, int frameSamples) {
    m_audioPts.store(pts, std::memory_order_relaxed);
    m_sampleRate.store(sampleRate > 0 ? sampleRate : 48000, std::memory_order_relaxed);
    m_frameSamples.store(frameSamples >= 0 ? frameSamples : 0, std::memory_order_relaxed);
}

inline void av::sync::AVSync::update(int64_t pts, int sampleRate, int frameSamples) {
    reset(pts, sampleRate, frameSamples);
}

inline int64_t av::sync::AVSync::audioPts() const {
    return m_audioPts.load(std::memory_order_relaxed);
}

inline int64_t av::sync::AVSync::audioTimeMs() const {
    return audioPtsToTimeMs(audioPts(), sampleRate());
}

inline int av::sync::AVSync::sampleRate() const {
    return m_sampleRate.load(std::memory_order_relaxed);
}

inline int av::sync::AVSync::frameSamples() const {
    return m_frameSamples.load(std::memory_order_relaxed);
}

inline bool av::sync::AVSync::hasAudioPts() const {
    return audioPts() >= 0;
}

inline int64_t av::sync::AVSync::audioPtsToTimeMs(int64_t pts, int sampleRate) {
    if (pts < 0 || sampleRate <= 0) {
        return -1;
    }
    return (pts * 1000) / sampleRate;
}

inline int64_t av::sync::AVSync::videoPts90kToTimeMs(int64_t pts90k) {
    if (pts90k < 0) {
        return -1;
    }
    return (pts90k * 1000) / 90000;
}

inline int64_t av::sync::AVSync::videoAudioSkewMs(int64_t videoPts90k, int64_t audioPts, int audioSampleRate) {
    const int64_t videoMs = videoPts90kToTimeMs(videoPts90k);
    const int64_t audioMs = audioPtsToTimeMs(audioPts, audioSampleRate);
    if (videoMs < 0 || audioMs < 0) {
        return (std::numeric_limits<int64_t>::min)();
    }
    return videoMs - audioMs;
}

inline bool av::sync::AVSync::shouldDropVideoFrameByAudioClock(int64_t videoPts90k,
                                                                int64_t audioPts,
                                                                int audioSampleRate,
                                                                int maxLeadMs,
                                                                int maxLagMs) {
    if (maxLeadMs < 0 || maxLagMs < 0) {
        return false;
    }

    const int64_t skewMs = videoAudioSkewMs(videoPts90k, audioPts, audioSampleRate);
    if (skewMs == (std::numeric_limits<int64_t>::min)()) {
        return false;
    }
    return skewMs > maxLeadMs || skewMs < -maxLagMs;
}

inline int av::sync::AVSync::suggestVideoRenderDelayMsByAudioClock(int64_t videoPts90k,
                                                                    int64_t audioPts,
                                                                    int audioSampleRate,
                                                                    int maxDelayMs) {
    if (maxDelayMs <= 0) {
        return 0;
    }

    const int64_t skewMs = videoAudioSkewMs(videoPts90k, audioPts, audioSampleRate);
    if (skewMs == (std::numeric_limits<int64_t>::min)() || skewMs <= 0) {
        return 0;
    }

    if (skewMs >= static_cast<int64_t>(maxDelayMs)) {
        return maxDelayMs;
    }
    return static_cast<int>(skewMs);
}
