#include "AudioPlayer.h"

#include "av/sync/AVSync.h"

#include <QCoreApplication>
#include <QDebug>
#include <QIODevice>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#ifdef _DEBUG
#pragma comment(lib, "Qt6Multimediad")
#else
#pragma comment(lib, "Qt6Multimedia")
#endif
#endif

namespace av::render {

AudioPlayer::AudioPlayer(std::size_t maxQueuedFrames)
    : m_clock(std::make_shared<av::sync::AVSync>()),
      m_maxQueuedFrames(maxQueuedFrames == 0 ? 1 : maxQueuedFrames) {}

AudioPlayer::~AudioPlayer() {
    stop();
}

bool AudioPlayer::configure(int sampleRate, int channels, int frameSamples) {
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

bool AudioPlayer::start(FrameCallback callback) {
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

void AudioPlayer::stop() {
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

bool AudioPlayer::enqueueFrame(capture::AudioFrame frame) {
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

bool AudioPlayer::popPlayedFrame(capture::AudioFrame& outFrame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playedFrames.empty()) {
        return false;
    }
    outFrame = std::move(m_playedFrames.front());
    m_playedFrames.pop_front();
    return true;
}

bool AudioPlayer::waitForPlayedFrame(capture::AudioFrame& outFrame, std::chrono::milliseconds timeout) {
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

QStringList AudioPlayer::availableOutputDevices() {
    QStringList names;
    for (const auto& output : QMediaDevices::audioOutputs()) {
        QString name = output.description().trimmed();
        if (name.isEmpty()) {
            name = QString::fromUtf8(output.id()).trimmed();
        }
        if (!name.isEmpty() && !names.contains(name, Qt::CaseInsensitive)) {
            names.append(name);
        }
    }
    return names;
}

bool AudioPlayer::setPreferredOutputDeviceName(const QString& deviceName) {
    const QString normalized = deviceName.trimmed();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_preferredOutputDeviceName.compare(normalized, Qt::CaseInsensitive) == 0) {
        return true;
    }
    m_preferredOutputDeviceName = normalized;
    return true;
}

QString AudioPlayer::preferredOutputDeviceName() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_preferredOutputDeviceName;
}

void AudioPlayer::setVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (volume < 0.0F) {
        m_volume = 0.0F;
    } else if (volume > 2.0F) {
        m_volume = 2.0F;
    } else {
        m_volume = volume;
    }
}

float AudioPlayer::volume() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_volume;
}

bool AudioPlayer::isRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

std::size_t AudioPlayer::queuedFrames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inputFrames.size();
}

int AudioPlayer::sampleRate() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sampleRate;
}

int AudioPlayer::channels() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_channels;
}

int AudioPlayer::frameSamples() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frameSamples;
}

AudioPlayerStats AudioPlayer::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

std::shared_ptr<av::sync::AVSync> AudioPlayer::clock() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_clock;
}

QAudioFormat AudioPlayer::buildDeviceFormat(int sampleRate, int channels) {
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

float AudioPlayer::sampleAt(const capture::AudioFrame& frame, int sourceChannel, double sourcePosition) {
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

float AudioPlayer::mixFrameSample(const capture::AudioFrame& frame, int targetChannel, int targetChannels, double sourcePosition) {
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

QByteArray AudioPlayer::frameToPcm(const capture::AudioFrame& frame, const QAudioFormat& format) {
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

void AudioPlayer::playbackLoop() {
    std::unique_ptr<QAudioSink> audioSink;
    QIODevice* audioDevice = nullptr;
    QAudioFormat deviceFormat;
    bool loggedFirstPlaybackFrame = false;
    auto lastReopenAttempt = std::chrono::steady_clock::time_point{};

    auto resolveOutputDevice = [](const QString& preferredDeviceName) {
        const auto outputs = QMediaDevices::audioOutputs();
        if (outputs.isEmpty()) {
            return QAudioDevice{};
        }

        const QString preferred = preferredDeviceName.trimmed();
        if (!preferred.isEmpty()) {
            for (const auto& output : outputs) {
                const QString description = output.description().trimmed();
                const QString idText = QString::fromUtf8(output.id()).trimmed();
                if (description.compare(preferred, Qt::CaseInsensitive) == 0 ||
                    idText.compare(preferred, Qt::CaseInsensitive) == 0) {
                    return output;
                }
            }
        }

        const QAudioDevice defaultOutput = QMediaDevices::defaultAudioOutput();
        if (!defaultOutput.isNull()) {
            return defaultOutput;
        }

        return outputs.front();
    };

    auto openOutputDevice = [&](const QString& preferredDeviceName) {
        if (audioSink != nullptr) {
            audioSink->stop();
            audioSink.reset();
            audioDevice = nullptr;
        }

        if (QCoreApplication::instance() == nullptr) {
            qWarning().noquote() << "[audio-player] fallback: no Qt application instance";
            return;
        }

        const QAudioDevice outputDevice = resolveOutputDevice(preferredDeviceName);
        if (outputDevice.isNull()) {
            qWarning().noquote() << "[audio-player] fallback: no audio output available";
            return;
        }

        const QAudioFormat desired = buildDeviceFormat(m_sampleRate, m_channels);
        deviceFormat = outputDevice.isFormatSupported(desired) ? desired : outputDevice.preferredFormat();
        if (!deviceFormat.isValid()) {
            qWarning().noquote() << "[audio-player] fallback: invalid output format";
            return;
        }

        audioSink = std::make_unique<QAudioSink>(outputDevice, deviceFormat);
        audioSink->setVolume(1.0F);
        audioSink->setBufferSize(std::max(8192, deviceFormat.bytesPerFrame() * m_frameSamples * 6));
        audioDevice = audioSink->start();
        if (audioDevice == nullptr) {
            qWarning().noquote() << "[audio-player] fallback: failed to start output device";
            audioSink.reset();
            return;
        }

        qInfo().noquote() << "[audio-player] output=" << outputDevice.description() << "sr=" << deviceFormat.sampleRate() << "ch=" << deviceFormat.channelCount() << "fmt=" << static_cast<int>(deviceFormat.sampleFormat());
    };

    auto tryReopenOutputDevice = [&](const QString& preferredDeviceName, const QString& reason, bool force) {
        constexpr auto kRetryInterval = std::chrono::milliseconds(1000);
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            lastReopenAttempt != std::chrono::steady_clock::time_point{} &&
            (now - lastReopenAttempt) < kRetryInterval) {
            return;
        }

        lastReopenAttempt = now;
        qWarning().noquote() << "[audio-player] reopen output device:" << reason;
        openOutputDevice(preferredDeviceName);
    };

    QString preferredOutputDeviceName;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        preferredOutputDeviceName = m_preferredOutputDeviceName;
    }
    openOutputDevice(preferredOutputDeviceName);

    for (;;) {
        capture::AudioFrame frame;
        FrameCallback callback;
        std::shared_ptr<av::sync::AVSync> clock;
        QString desiredOutputDeviceName;

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
            desiredOutputDeviceName = m_preferredOutputDeviceName;
        }

        if (preferredOutputDeviceName.compare(desiredOutputDeviceName, Qt::CaseInsensitive) != 0) {
            preferredOutputDeviceName = desiredOutputDeviceName;
            openOutputDevice(preferredOutputDeviceName);
        }

        if (audioSink != nullptr && audioSink->state() == QAudio::StoppedState && !m_stopRequested) {
            const QAudio::Error sinkError = audioSink->error();
            if (sinkError == QAudio::UnderrunError) {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.underruns;
            }
            tryReopenOutputDevice(preferredOutputDeviceName,
                                  QStringLiteral("stopped before write, error=%1").arg(static_cast<int>(sinkError)),
                                  false);
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
                    const qint64 freeBytes = audioSink != nullptr ? audioSink->bytesFree() : 0;
                    if (freeBytes <= 0) {
                        const QAudio::State sinkState = audioSink != nullptr ? audioSink->state() : QAudio::StoppedState;
                        if (sinkState == QAudio::StoppedState) {
                            const QAudio::Error sinkError = audioSink != nullptr ? audioSink->error() : QAudio::OpenError;
                            if (sinkError == QAudio::UnderrunError) {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                ++m_stats.underruns;
                            }
                            tryReopenOutputDevice(preferredOutputDeviceName,
                                                  QStringLiteral("stopped while waiting for buffer space, error=%1").arg(static_cast<int>(sinkError)),
                                                  false);
                            if (audioSink == nullptr || audioDevice == nullptr) {
                                break;
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        continue;
                    }

                    const qsizetype bytesToWrite = static_cast<qsizetype>(
                        std::min<qint64>(freeBytes, static_cast<qint64>(pcm.size() - offset)));
                    const qint64 written = audioDevice->write(pcm.constData() + offset, bytesToWrite);
                    if (written > 0) {
                        offset += written;
                        continue;
                    }

                    const QAudio::State sinkState = audioSink->state();
                    if (sinkState == QAudio::StoppedState) {
                        const QAudio::Error sinkError = audioSink->error();
                        if (sinkError == QAudio::UnderrunError) {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            ++m_stats.underruns;
                        }
                        tryReopenOutputDevice(preferredOutputDeviceName,
                                              QStringLiteral("stopped while writing, error=%1").arg(static_cast<int>(sinkError)),
                                              false);
                        if (audioSink == nullptr || audioDevice == nullptr) {
                            break;
                        }
                        continue;
                    }

                    if (written < 0) {
                        tryReopenOutputDevice(preferredOutputDeviceName,
                                              QStringLiteral("write returned %1").arg(written),
                                              false);
                        if (audioSink == nullptr || audioDevice == nullptr) {
                            break;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        } else {
            tryReopenOutputDevice(preferredOutputDeviceName, QStringLiteral("output unavailable"), false);
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

capture::AudioFrame AudioPlayer::applyGain(capture::AudioFrame frame) const {
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

void AudioPlayer::clearQueuesLocked() {
    m_inputFrames.clear();
    m_playedFrames.clear();
}

bool AudioPlayer::validateFrameLocked(const capture::AudioFrame& frame) const {
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

bool runAudioPlayerSelfCheck() {
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

}  // namespace av::render
