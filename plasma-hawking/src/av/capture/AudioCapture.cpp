#include "AudioCapture.h"

#include <QtMultimedia/QAudioFormat>
#include <QtMultimedia/QAudioSource>
#include <QtMultimedia/QMediaDevices>

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QIODevice>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#ifdef _DEBUG
#pragma comment(lib, "Qt6Multimediad")
#else
#pragma comment(lib, "Qt6Multimedia")
#endif
#endif

namespace av::capture {

namespace {
constexpr int kTargetSampleRate = 48000;
constexpr int kTargetChannels = 1;
constexpr int kTargetFrameSamples = 960;
constexpr float kSyntheticToneHz = 440.0F;
constexpr float kSyntheticAmplitude = 0.05F;
constexpr double kPi = 3.14159265358979323846;

bool allowSyntheticCapture() {
    return qEnvironmentVariableIntValue("MEETING_SYNTHETIC_AUDIO") != 0;
}

QAudioFormat chooseFormat(const QAudioDevice& device) {
    QAudioFormat format;
    format.setSampleRate(kTargetSampleRate);
    format.setChannelCount(kTargetChannels);
    format.setChannelConfig(QAudioFormat::ChannelConfigMono);
    format.setSampleFormat(QAudioFormat::Float);
    if (device.isFormatSupported(format)) {
        return format;
    }

    format.setSampleFormat(QAudioFormat::Int16);
    if (device.isFormatSupported(format)) {
        return format;
    }

    const QAudioFormat preferred = device.preferredFormat();
    if (preferred.isValid()) {
        return preferred;
    }

    return format;
}

float clampSample(float value) {
    if (value > 1.0F) {
        return 1.0F;
    }
    if (value < -1.0F) {
        return -1.0F;
    }
    return value;
}

QString audioInputDeviceName(const QAudioDevice& device) {
    const QString description = device.description().trimmed();
    if (!description.isEmpty()) {
        return description;
    }
    return QString::fromUtf8(device.id()).trimmed();
}

QAudioDevice resolvePreferredInputDevice(const QString& preferredDeviceName) {
    const auto inputs = QMediaDevices::audioInputs();
    if (inputs.isEmpty()) {
        return {};
    }

    const QString preferred = preferredDeviceName.trimmed();
    if (!preferred.isEmpty()) {
        for (const auto& input : inputs) {
            const QString name = audioInputDeviceName(input);
            if (name.compare(preferred, Qt::CaseInsensitive) == 0) {
                return input;
            }
        }
    }

    const QAudioDevice defaultInput = QMediaDevices::defaultAudioInput();
    if (!defaultInput.isNull()) {
        return defaultInput;
    }

    return inputs.front();
}
}  // namespace

struct AudioCapture::Impl {
    class CaptureDevice final : public QIODevice {
    public:
        explicit CaptureDevice(Impl* owner)
            : m_owner(owner) {}

        bool open(OpenMode mode) {
            return QIODevice::open(mode | QIODevice::Unbuffered);
        }

    protected:
        qint64 readData(char*, qint64) override {
            return 0;
        }

        qint64 writeData(const char* data, qint64 len) override;

    private:
        Impl* m_owner{nullptr};
    };

    explicit Impl(AudioCapture* owner)
        : owner(owner) {}

    void startSyntheticDevice() {
        syntheticMode = true;
        inputSampleRate = kTargetSampleRate;
        inputChannels = kTargetChannels;
        inputBytesPerSample = static_cast<int>(sizeof(float));
        resampleRatio = 1.0;
        nextPts = 0;
        running = true;
        error.clear();
        qWarning().noquote() << "[audio-capture] synthetic mode enabled";
        syntheticThread = std::thread([this]() {
            syntheticLoop();
        });
    }

    void syntheticLoop() {
        double phase = 0.0;
        const double phaseStep = (2.0 * kPi * static_cast<double>(kSyntheticToneHz)) /
                                 static_cast<double>(kTargetSampleRate);
        int64_t pts = 0;
        auto nextWake = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_acquire)) {
            AudioFrame frame;
            frame.sampleRate = kTargetSampleRate;
            frame.channels = kTargetChannels;
            frame.pts = pts;
            frame.samples.resize(kTargetFrameSamples);
            for (float& sample : frame.samples) {
                sample = static_cast<float>(std::sin(phase) * static_cast<double>(kSyntheticAmplitude));
                phase += phaseStep;
                if (phase >= 2.0 * kPi) {
                    phase -= 2.0 * kPi;
                }
            }
            if (owner != nullptr) {
                (void)owner->m_ringBuffer.push(std::move(frame));
            }
            pts += kTargetFrameSamples;
            nextWake += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(nextWake);
        }
    }

    void startDevice() {
        if (allowSyntheticCapture()) {
            startSyntheticDevice();
            return;
        }

        if (QCoreApplication::instance() == nullptr) {
            error = "no Qt application instance";
            return;
        }

        device = resolvePreferredInputDevice(preferredDeviceName);
        if (device.isNull()) {
            error = "no default audio input";
            return;
        }

        format = chooseFormat(device);
        if (!format.isValid()) {
            error = "invalid audio input format";
            return;
        }

        audioSource = std::make_unique<QAudioSource>(device, format);
        if (!audioSource || audioSource->isNull()) {
            error = "failed to create audio source";
            audioSource.reset();
            return;
        }

        const int bytesPerFrame = std::max(1, format.bytesPerFrame());
        audioSource->setBufferSize(bytesPerFrame * kTargetFrameSamples * 4);

        captureDevice = std::make_unique<CaptureDevice>(this);
        if (!captureDevice->open(QIODevice::WriteOnly)) {
            error = "failed to open audio capture device";
            captureDevice.reset();
            audioSource.reset();
            return;
        }

        audioSource->start(captureDevice.get());
        if (audioSource->error() != QtAudio::NoError) {
            error = "audio source start failed";
            audioSource->stop();
            captureDevice.reset();
            audioSource.reset();
            return;
        }

        inputSampleRate = format.sampleRate() > 0 ? format.sampleRate() : kTargetSampleRate;
        inputChannels = std::max(1, format.channelCount());
        inputBytesPerSample = std::max(1, format.bytesPerSample());
        resampleRatio = static_cast<double>(inputSampleRate) / static_cast<double>(kTargetSampleRate);
        nextPts = 0;
        running = true;
    }

    void stopDevice() {
        running = false;
        if (syntheticThread.joinable()) {
            syntheticThread.join();
        }
        syntheticMode = false;
        if (audioSource) {
            audioSource->stop();
        }
        if (captureDevice) {
            captureDevice->close();
        }
        captureDevice.reset();
        audioSource.reset();
        pendingBytes.clear();
        pendingSamples.clear();
        inputCursor = 0.0;
        nextPts = 0;
    }

    void ingestBytes(const char* data, qint64 len) {
        if (!running || data == nullptr || len <= 0 || !format.isValid()) {
            return;
        }

        pendingBytes.append(data, static_cast<int>(len));
        const int frameBytes = format.bytesPerFrame();
        if (frameBytes <= 0) {
            pendingBytes.clear();
            return;
        }

        while (pendingBytes.size() >= frameBytes) {
            appendFrameSamples(pendingBytes.constData(), frameBytes);
            pendingBytes.remove(0, frameBytes);
        }

        drainFrames();
    }

    void appendFrameSamples(const char* frameBytes, int frameBytesSize) {
        if (frameBytes == nullptr || frameBytesSize <= 0 || inputChannels <= 0 || inputBytesPerSample <= 0) {
            return;
        }

        const int samplesPerChannel = frameBytesSize / (inputChannels * inputBytesPerSample);
        const int usableBytes = samplesPerChannel * inputChannels * inputBytesPerSample;
        if (samplesPerChannel <= 0 || usableBytes <= 0) {
            return;
        }

        const int bytesPerFrame = inputChannels * inputBytesPerSample;
        for (int sampleIndex = 0; sampleIndex < samplesPerChannel; ++sampleIndex) {
            const char* sampleFrame = frameBytes + sampleIndex * bytesPerFrame;
            float mono = 0.0F;
            for (int channel = 0; channel < inputChannels; ++channel) {
                const char* samplePtr = sampleFrame + channel * inputBytesPerSample;
                mono += format.normalizedSampleValue(samplePtr);
            }
            mono /= static_cast<float>(inputChannels);
            pendingSamples.push_back(clampSample(mono));
        }
    }

    bool canProduceFrame() const {
        if (pendingSamples.empty()) {
            return false;
        }
        const double lastPos = inputCursor + static_cast<double>(kTargetFrameSamples - 1) * resampleRatio;
        const std::size_t lastIndex = static_cast<std::size_t>(std::floor(lastPos));
        return lastIndex + 1 < pendingSamples.size();
    }

    float sampleAt(double position) const {
        const std::size_t index = static_cast<std::size_t>(std::floor(position));
        const double fraction = position - static_cast<double>(index);
        const float a = pendingSamples[index];
        const float b = pendingSamples[index + 1];
        return static_cast<float>(a + (b - a) * fraction);
    }

    void dropConsumedSamples() {
        const std::size_t dropCount = static_cast<std::size_t>(std::floor(inputCursor));
        if (dropCount == 0) {
            return;
        }

        const std::size_t safeDrop = std::min(dropCount, pendingSamples.size() > 1 ? pendingSamples.size() - 1 : pendingSamples.size());
        for (std::size_t i = 0; i < safeDrop; ++i) {
            pendingSamples.pop_front();
        }
        inputCursor -= static_cast<double>(safeDrop);
    }

    void drainFrames() {
        while (running && owner != nullptr && canProduceFrame()) {
            AudioFrame frame;
            frame.sampleRate = kTargetSampleRate;
            frame.channels = kTargetChannels;
            frame.pts = nextPts;
            frame.samples.resize(kTargetFrameSamples);

            for (int i = 0; i < kTargetFrameSamples; ++i) {
                const double position = inputCursor + static_cast<double>(i) * resampleRatio;
                frame.samples[static_cast<std::size_t>(i)] = clampSample(sampleAt(position));
            }

            if (!loggedFirstFrame) {
                qInfo().noquote() << "[audio-capture] first frame pts=" << frame.pts << "samples=" << frame.samples.size();
                loggedFirstFrame = true;
            }
            owner->m_ringBuffer.push(std::move(frame));
            nextPts += kTargetFrameSamples;

            inputCursor += static_cast<double>(kTargetFrameSamples) * resampleRatio;
            dropConsumedSamples();
        }
    }

    AudioCapture* owner{nullptr};
    QAudioDevice device;
    QAudioFormat format;
    std::unique_ptr<QAudioSource> audioSource;
    std::unique_ptr<CaptureDevice> captureDevice;
    std::deque<float> pendingSamples;
    QByteArray pendingBytes;
    QString preferredDeviceName;
    std::thread syntheticThread;
    int inputSampleRate{kTargetSampleRate};
    int inputChannels{kTargetChannels};
    int inputBytesPerSample{2};
    double resampleRatio{1.0};
    double inputCursor{0.0};
    int64_t nextPts{0};
    std::atomic<bool> running{false};
    bool syntheticMode{false};
    bool loggedFirstFrame{false};
    std::string error;
};

qint64 AudioCapture::Impl::CaptureDevice::writeData(const char* data, qint64 len) {
    if (m_owner == nullptr) {
        return len;
    }
    m_owner->ingestBytes(data, len);
    return len;
}

AudioCapture::AudioCapture(std::size_t ringCapacity)
    : m_ringBuffer(ringCapacity) {}

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        return false;
    }

    m_ringBuffer.reset();
    m_impl = std::make_unique<Impl>(this);
    m_impl->preferredDeviceName = m_preferredDeviceName;
    m_impl->startDevice();
    if (!m_impl->running) {
        m_impl.reset();
        m_running.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

void AudioCapture::stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    if (m_impl) {
        m_impl->stopDevice();
        m_impl.reset();
    }

    m_ringBuffer.close();
}

bool AudioCapture::isRunning() const {
    return m_running.load(std::memory_order_relaxed);
}

QStringList AudioCapture::availableInputDevices() {
    QStringList names;
    for (const auto& input : QMediaDevices::audioInputs()) {
        const QString name = audioInputDeviceName(input);
        if (!name.isEmpty() && !names.contains(name, Qt::CaseInsensitive)) {
            names.append(name);
        }
    }
    return names;
}

bool AudioCapture::setPreferredDeviceName(const QString& deviceName) {
    const QString normalized = deviceName.trimmed();
    if (m_preferredDeviceName == normalized) {
        return true;
    }

    const QString previousPreferred = m_preferredDeviceName;
    m_preferredDeviceName = normalized;

    if (!m_impl) {
        return true;
    }

    const QString previousImplPreferred = m_impl->preferredDeviceName;
    m_impl->preferredDeviceName = normalized;

    if (!m_impl->running) {
        return true;
    }

    m_impl->stopDevice();
    m_impl->startDevice();
    if (m_impl->running) {
        return true;
    }

    // Rollback to previous device preference if switching fails.
    m_preferredDeviceName = previousPreferred;
    m_impl->stopDevice();
    m_impl->preferredDeviceName = previousImplPreferred;
    m_impl->startDevice();
    return false;
}

QString AudioCapture::preferredDeviceName() const {
    return m_preferredDeviceName;
}
bool AudioCapture::pushCapturedFrame(AudioFrame frame) {
    if (!isRunning()) {
        return false;
    }
    return m_ringBuffer.push(std::move(frame));
}

bool AudioCapture::popFrameForEncode(AudioFrame& outFrame, std::chrono::milliseconds timeout) {
    if (!isRunning()) {
        return false;
    }
    return m_ringBuffer.popWait(outFrame, timeout);
}

}  // namespace av::capture








