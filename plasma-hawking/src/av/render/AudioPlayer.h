#pragma once

#include "av/capture/AudioCapture.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtMultimedia/QAudioFormat>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace av::sync {
class AVSync;
}

namespace av::render {

struct AudioPlayerStats {
    std::uint64_t enqueuedFrames{0};
    std::uint64_t playedFrames{0};
    std::uint64_t droppedFrames{0};
    std::uint64_t underruns{0};
};

class AudioPlayer {
public:
    using FrameCallback = std::function<void(const capture::AudioFrame&)>;

    explicit AudioPlayer(std::size_t maxQueuedFrames = 8);
    ~AudioPlayer();

    bool configure(int sampleRate, int channels, int frameSamples = 960);
    bool start(FrameCallback callback = {});
    void stop();

    bool enqueueFrame(capture::AudioFrame frame);
    bool popPlayedFrame(capture::AudioFrame& outFrame);
    bool waitForPlayedFrame(capture::AudioFrame& outFrame, std::chrono::milliseconds timeout);

    static QStringList availableOutputDevices();
    bool setPreferredOutputDeviceName(const QString& deviceName);
    QString preferredOutputDeviceName() const;

    void setVolume(float volume);
    float volume() const;

    bool isRunning() const;
    std::size_t queuedFrames() const;
    int sampleRate() const;
    int channels() const;
    int frameSamples() const;
    AudioPlayerStats stats() const;

    std::shared_ptr<av::sync::AVSync> clock() const;

private:
    using FrameQueue = std::deque<capture::AudioFrame>;

    static QAudioFormat buildDeviceFormat(int sampleRate, int channels);
    static float sampleAt(const capture::AudioFrame& frame, int sourceChannel, double sourcePosition);
    static float mixFrameSample(const capture::AudioFrame& frame, int targetChannel, int targetChannels, double sourcePosition);
    static QByteArray frameToPcm(const capture::AudioFrame& frame, const QAudioFormat& format);

    void playbackLoop();
    capture::AudioFrame applyGain(capture::AudioFrame frame) const;
    void clearQueuesLocked();
    bool validateFrameLocked(const capture::AudioFrame& frame) const;

    mutable std::mutex m_mutex;
    std::condition_variable m_inputCv;
    std::condition_variable m_outputCv;
    FrameQueue m_inputFrames;
    FrameQueue m_playedFrames;
    std::thread m_worker;
    FrameCallback m_callback;
    std::shared_ptr<av::sync::AVSync> m_clock;
    AudioPlayerStats m_stats;
    std::size_t m_maxQueuedFrames;
    int m_sampleRate{48000};
    int m_channels{1};
    int m_frameSamples{960};
    float m_volume{1.0F};
    QString m_preferredOutputDeviceName;
    bool m_running{false};
    bool m_stopRequested{false};
};

bool runAudioPlayerSelfCheck();

}  // namespace av::render
