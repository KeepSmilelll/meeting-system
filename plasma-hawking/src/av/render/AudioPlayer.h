#pragma once

#include "av/capture/AudioCapture.h"
#include "av/sync/AVSync.h"

#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioFormat>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QMediaDevices>

#include <QCoreApplication>
#include <QDebug>
#include <QByteArray>
#include <QIODevice>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

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
    bool m_running{false};
    bool m_stopRequested{false};
};

bool runAudioPlayerSelfCheck();

}  // namespace av::render

inline av::render::AudioPlayer::AudioPlayer(std::size_t maxQueuedFrames)
    : m_clock(std::make_shared<av::sync::AVSync>()),
      m_maxQueuedFrames(maxQueuedFrames == 0 ? 1 : maxQueuedFrames) {}

inline av::render::AudioPlayer::~AudioPlayer() {
    stop();
}

inline bool av::render::AudioPlayer::configure(int sampleRate, int channels, int frameSamples) {
    if (sampleRate <= 0 || frameSamples <= 0 || (channels != 1 && channels != 2)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) {
        return false;
    }

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_frameSamples = frameSamples;
    if (m_clock) {
        m_clock->reset(-1, sampleRate, frameSamples);
    }
    return true;
}

inline bool av::render::AudioPlayer::start(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) {
        return false;
    }

    clearQueuesLocked();
    m_callback = std::move(callback);
    m_stopRequested = false;
    m_running = true;
    if (m_clock) {
        m_clock->reset(-1, m_sampleRate, m_frameSamples);
    }
    m_worker = std::thread(&AudioPlayer::playbackLoop, this);
    return true;
}

inline void av::render::AudioPlayer::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running && !m_worker.joinable()) {
            return;
        }
        m_stopRequested = true;
        clearQueuesLocked();
    }

    m_inputCv.notify_all();
    m_outputCv.notify_all();

    if (m_worker.joinable()) {
        m_worker.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_running = false;
    m_stopRequested = false;
    m_callback = {};
}

inline bool av::render::AudioPlayer::enqueueFrame(capture::AudioFrame frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running || m_stopRequested) {
        return false;
    }
    if (!validateFrameLocked(frame)) {
        return false;
    }

    if (m_inputFrames.size() >= m_maxQueuedFrames) {
        m_inputFrames.pop_front();
        ++m_stats.droppedFrames;
    }

    m_inputFrames.push_back(std::move(frame));
    ++m_stats.enqueuedFrames;
    m_inputCv.notify_one();
    return true;
}

inline bool av::render::AudioPlayer::popPlayedFrame(capture::AudioFrame& outFrame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playedFrames.empty()) {
        return false;
    }
    outFrame = std::move(m_playedFrames.front());
    m_playedFrames.pop_front();
    return true;
}

inline bool av::render::AudioPlayer::waitForPlayedFrame(capture::AudioFrame& outFrame, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_outputCv.wait_for(lock, timeout, [this] { return !m_playedFrames.empty() || (!m_running && m_stopRequested); })) {
        return false;
    }
    if (m_playedFrames.empty()) {
        return false;
    }
    outFrame = std::move(m_playedFrames.front());
    m_playedFrames.pop_front();
    return true;
}

inline void av::render::AudioPlayer::setVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (volume < 0.0F) {
        m_volume = 0.0F;
    } else if (volume > 2.0F) {
        m_volume = 2.0F;
    } else {
        m_volume = volume;
    }
}

inline float av::render::AudioPlayer::volume() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_volume;
}

inline bool av::render::AudioPlayer::isRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

inline std::size_t av::render::AudioPlayer::queuedFrames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inputFrames.size();
}

inline int av::render::AudioPlayer::sampleRate() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sampleRate;
}

inline int av::render::AudioPlayer::channels() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_channels;
}

inline int av::render::AudioPlayer::frameSamples() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frameSamples;
}

inline av::render::AudioPlayerStats av::render::AudioPlayer::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

inline std::shared_ptr<av::sync::AVSync> av::render::AudioPlayer::clock() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_clock;
}

inline QAudioFormat av::render::AudioPlayer::buildDeviceFormat(int sampleRate, int channels) {
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

inline float av::render::AudioPlayer::sampleAt(const capture::AudioFrame& frame, int sourceChannel, double sourcePosition) {
    const int sourceChannels = frame.channels > 0 ? frame.channels : 1;
    const std::size_t totalSamples = frame.samples.size();
    const int sourceFrames = sourceChannels > 0 ? static_cast<int>(totalSamples / static_cast<std::size_t>(sourceChannels)) : 0;
    if (sourceFrames <= 0 || frame.samples.empty()) {
        return 0.0F;
    }

    const double clamped = std::clamp(sourcePosition, 0.0, static_cast<double>(sourceFrames - 1));
    const int index0 = static_cast<int>(clamped);
    const int index1 = std::min(index0 + 1, sourceFrames - 1);
    const float frac = static_cast<float>(clamped - static_cast<double>(index0));
    const int channel = std::clamp(sourceChannel, 0, sourceChannels - 1);
    const std::size_t offset0 = static_cast<std::size_t>(index0) * static_cast<std::size_t>(sourceChannels) + static_cast<std::size_t>(channel);
    const std::size_t offset1 = static_cast<std::size_t>(index1) * static_cast<std::size_t>(sourceChannels) + static_cast<std::size_t>(channel);
    const float s0 = frame.samples[offset0];
    const float s1 = frame.samples[offset1];
    return s0 + (s1 - s0) * frac;
}

inline float av::render::AudioPlayer::mixFrameSample(const capture::AudioFrame& frame, int targetChannel, int targetChannels, double sourcePosition) {
    const int sourceChannels = frame.channels > 0 ? frame.channels : 1;
    if (sourceChannels <= 1) {
        return sampleAt(frame, 0, sourcePosition);
    }

    if (targetChannels <= 1) {
        float mixed = 0.0F;
        for (int sourceChannel = 0; sourceChannel < sourceChannels; ++sourceChannel) {
            mixed += sampleAt(frame, sourceChannel, sourcePosition);
        }
        return mixed / static_cast<float>(sourceChannels);
    }

    const int mappedChannel = std::min(targetChannel, sourceChannels - 1);
    return sampleAt(frame, mappedChannel, sourcePosition);
}

inline QByteArray av::render::AudioPlayer::frameToPcm(const capture::AudioFrame& frame, const QAudioFormat& format) {
    const int sourceChannels = frame.channels > 0 ? frame.channels : 1;
    const int sourceFrames = sourceChannels > 0
                                 ? static_cast<int>(frame.samples.size() / static_cast<std::size_t>(sourceChannels))
                                 : 0;
    const int targetChannels = std::max(1, format.channelCount());
    const int sourceRate = frame.sampleRate > 0 ? frame.sampleRate : 48000;
    const int targetRate = format.sampleRate() > 0 ? format.sampleRate() : sourceRate;
    const int bytesPerSample = std::max(1, format.bytesPerSample());
    if (sourceFrames <= 0 || targetRate <= 0) {
        return {};
    }

    const int targetFrames = std::max(1, static_cast<int>(std::lround(static_cast<double>(sourceFrames) * static_cast<double>(targetRate) / static_cast<double>(sourceRate))));
    QByteArray pcm;
    pcm.resize(targetFrames * targetChannels * bytesPerSample);

    char* out = pcm.data();
    const auto sampleFormat = format.sampleFormat();
    for (int i = 0; i < targetFrames; ++i) {
        const double sourcePosition = static_cast<double>(i) * static_cast<double>(sourceRate) / static_cast<double>(targetRate);
        for (int channel = 0; channel < targetChannels; ++channel) {
            const float sample = std::clamp(mixFrameSample(frame, channel, targetChannels, sourcePosition), -1.0F, 1.0F);
            switch (sampleFormat) {
            case QAudioFormat::Int16: {
                const qint16 value = static_cast<qint16>(std::lround(sample * 32767.0F));
                std::memcpy(out, &value, sizeof(value));
                out += sizeof(value);
                break;
            }
            case QAudioFormat::Int32: {
                const qint32 value = static_cast<qint32>(std::lround(sample * 2147483647.0F));
                std::memcpy(out, &value, sizeof(value));
                out += sizeof(value);
                break;
            }
            case QAudioFormat::Float: {
                const float value = sample;
                std::memcpy(out, &value, sizeof(value));
                out += sizeof(value);
                break;
            }
            case QAudioFormat::UInt8: {
                const quint8 value = static_cast<quint8>(std::lround((sample + 1.0F) * 127.5F));
                std::memcpy(out, &value, sizeof(value));
                out += sizeof(value);
                break;
            }
            default: {
                const qint16 value = static_cast<qint16>(std::lround(sample * 32767.0F));
                std::memcpy(out, &value, sizeof(value));
                out += sizeof(value);
                break;
            }
            }
        }
    }
    return pcm;
}
inline void av::render::AudioPlayer::playbackLoop() {
    std::unique_ptr<QAudioSink> audioSink;
    QIODevice* audioDevice = nullptr;
    QAudioFormat deviceFormat;
    bool loggedFirstPlaybackFrame = false;

    if (QCoreApplication::instance() != nullptr) {
        const QAudioDevice defaultOutput = QMediaDevices::defaultAudioOutput();
        if (!defaultOutput.isNull()) {
            const QAudioFormat desired = buildDeviceFormat(m_sampleRate, m_channels);
            deviceFormat = defaultOutput.isFormatSupported(desired) ? desired : defaultOutput.preferredFormat();
            if (deviceFormat.isValid()) {
                audioSink = std::make_unique<QAudioSink>(defaultOutput, deviceFormat);
                audioSink->setVolume(1.0F);
                audioSink->setBufferSize(std::max(4096, deviceFormat.bytesPerFrame() * m_frameSamples * 4));
                audioDevice = audioSink->start();
                if (audioDevice == nullptr) {
                    qWarning().noquote() << "[audio-player] fallback: failed to start default output device";
                    audioSink.reset();
                } else {
                    qInfo().noquote() << "[audio-player] output=" << defaultOutput.description() << "sr=" << deviceFormat.sampleRate() << "ch=" << deviceFormat.channelCount() << "fmt=" << static_cast<int>(deviceFormat.sampleFormat());
                }
            } else {
                qWarning().noquote() << "[audio-player] fallback: invalid output format";
            }
        } else {
            qWarning().noquote() << "[audio-player] fallback: no default audio output";
        }
    } else {
        qWarning().noquote() << "[audio-player] fallback: no Qt application instance";
    }

    for (;;) {
        capture::AudioFrame frame;
        FrameCallback callback;
        std::shared_ptr<av::sync::AVSync> clock;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_inputCv.wait(lock, [this] { return m_stopRequested || !m_inputFrames.empty(); });
            if (m_stopRequested && m_inputFrames.empty()) {
                m_running = false;
                m_outputCv.notify_all();
                break;
            }

            frame = std::move(m_inputFrames.front());
            m_inputFrames.pop_front();
            callback = m_callback;
            clock = m_clock;
        }

        frame = applyGain(std::move(frame));

        if (!loggedFirstPlaybackFrame) {
            qInfo().noquote() << "[audio-player] first frame pts=" << frame.pts << "samples=" << frame.samples.size();
            loggedFirstPlaybackFrame = true;
        }

        if (audioSink != nullptr && audioDevice != nullptr) {
            const QByteArray pcm = frameToPcm(frame, deviceFormat);
            if (!pcm.isEmpty()) {
                qsizetype offset = 0;
                while (offset < pcm.size() && !m_stopRequested) {
                    const qint64 written = audioDevice->write(pcm.constData() + offset, pcm.size() - offset);
                    if (written > 0) {
                        offset += written;
                        continue;
                    }

                    if (audioSink->state() == QAudio::StoppedState) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        const int channels = frame.channels > 0 ? frame.channels : 1;
        const std::size_t totalSamples = frame.samples.size();
        const int samplesPerChannel = channels > 0 ? static_cast<int>(totalSamples / static_cast<std::size_t>(channels)) : 0;
        if (clock && samplesPerChannel > 0) {
            clock->update(frame.pts + samplesPerChannel, frame.sampleRate, samplesPerChannel);
        }

        if (callback) {
            callback(frame);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_playedFrames.push_back(std::move(frame));
            if (m_playedFrames.size() > m_maxQueuedFrames) {
                m_playedFrames.pop_front();
            }
            ++m_stats.playedFrames;
        }
        m_outputCv.notify_one();
    }

    if (audioSink != nullptr) {
        audioSink->stop();
    }
}

inline av::capture::AudioFrame av::render::AudioPlayer::applyGain(capture::AudioFrame frame) const {
    const float gain = volume();
    if (gain == 1.0F) {
        return frame;
    }

    for (float& sample : frame.samples) {
        sample *= gain;
        if (sample > 1.0F) {
            sample = 1.0F;
        } else if (sample < -1.0F) {
            sample = -1.0F;
        }
    }
    return frame;
}

inline void av::render::AudioPlayer::clearQueuesLocked() {
    m_inputFrames.clear();
    m_playedFrames.clear();
}

inline bool av::render::AudioPlayer::validateFrameLocked(const capture::AudioFrame& frame) const {
    if (frame.sampleRate != m_sampleRate || frame.channels != m_channels) {
        return false;
    }
    if (frame.samples.empty()) {
        return false;
    }
    if ((frame.samples.size() % static_cast<std::size_t>(m_channels)) != 0) {
        return false;
    }
    return true;
}

inline bool av::render::runAudioPlayerSelfCheck() {
    AudioPlayer player(4);
    if (!player.configure(48000, 1, 960)) {
        return false;
    }
    player.setVolume(0.5F);

    if (!player.start()) {
        return false;
    }

    capture::AudioFrame frame;
    frame.sampleRate = 48000;
    frame.channels = 1;
    frame.pts = 0;
    frame.samples.assign(960, 0.8F);

    if (!player.enqueueFrame(frame)) {
        player.stop();
        return false;
    }

    capture::AudioFrame played;
    if (!player.waitForPlayedFrame(played, std::chrono::milliseconds(200))) {
        player.stop();
        return false;
    }

    const auto clock = player.clock();
    const bool ok = played.samples.size() == frame.samples.size() &&
                    played.samples.front() > 0.39F &&
                    played.samples.front() < 0.41F &&
                    played.samples[1] > 0.39F &&
                    played.samples[1] < 0.41F &&
                    clock &&
                    clock->hasAudioPts() &&
                    clock->audioPts() == 960 &&
                    clock->audioTimeMs() == 20;
    player.stop();
    return ok;
}





