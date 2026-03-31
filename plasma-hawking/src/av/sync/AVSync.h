#pragma once

#include <atomic>
#include <cstdint>

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
    const int64_t pts = audioPts();
    const int rate = sampleRate();
    if (pts < 0 || rate <= 0) {
        return -1;
    }
    return (pts * 1000) / rate;
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
