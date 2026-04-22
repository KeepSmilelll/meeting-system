#include "CameraCapture.h"

#include "av/FFmpegUtils.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QThread>
#include <QtMultimedia/QCamera>
#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dshow.h>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "Strmiids.lib")
#endif

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace av::capture {

namespace {

QString normalizeDeviceSelection(const QString& selection) {
    return selection.trimmed();
}

QString deviceNameForSelection(const QCameraDevice& device) {
    const QString description = normalizeDeviceSelection(device.description());
    if (!description.isEmpty()) {
        return description;
    }
    return QString::fromUtf8(device.id()).trimmed();
}

void appendUniqueDeviceName(QStringList& names, const QString& name) {
    const QString normalized = normalizeDeviceSelection(name);
    if (normalized.isEmpty() || names.contains(normalized, Qt::CaseInsensitive)) {
        return;
    }
    names.append(normalized);
}

bool deviceMatchesSelection(const QCameraDevice& device, const QString& selection) {
    const QString trimmedSelection = normalizeDeviceSelection(selection);
    if (trimmedSelection.isEmpty()) {
        return false;
    }

    const QString description = device.description().trimmed();
    const QString identifier = QString::fromUtf8(device.id()).trimmed();
    return description.compare(trimmedSelection, Qt::CaseInsensitive) == 0 ||
           identifier.compare(trimmedSelection, Qt::CaseInsensitive) == 0 ||
           description.contains(trimmedSelection, Qt::CaseInsensitive) ||
           identifier.contains(trimmedSelection, Qt::CaseInsensitive);
}

QCameraDevice findDeviceBySelection(const QString& selection) {
    if (selection.trimmed().isEmpty()) {
        return {};
    }

    const auto inputs = QMediaDevices::videoInputs();
    for (const auto& input : inputs) {
        if (deviceMatchesSelection(input, selection)) {
            return input;
        }
    }
    return {};
}

QStringList qtAvailableDeviceNames() {
    QStringList names;
    const auto inputs = QMediaDevices::videoInputs();
    for (const auto& input : inputs) {
        appendUniqueDeviceName(names, deviceNameForSelection(input));
    }
    return names;
}

#ifdef _WIN32
QString formatHResult(HRESULT hr) {
    return QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(static_cast<unsigned long>(hr)), 8, 16, QLatin1Char('0'));
}

template <typename T>
void releaseComPtr(T*& ptr) {
    if (ptr != nullptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

QStringList dshowAvailableDeviceNames(QString* error = nullptr) {
    QStringList names;

    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
        if (error != nullptr) {
            *error = QStringLiteral("CoInitializeEx failed: %1").arg(formatHResult(initResult));
        }
        return names;
    }

    ICreateDevEnum* devEnum = nullptr;
    IEnumMoniker* enumMoniker = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ICreateDevEnum,
                                  reinterpret_cast<void**>(&devEnum));
    if (FAILED(hr) || devEnum == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("CoCreateInstance(ICreateDevEnum) failed: %1").arg(formatHResult(hr));
        }
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return names;
    }

    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    if (hr == S_FALSE || enumMoniker == nullptr) {
        if (error != nullptr) {
            error->clear();
        }
        releaseComPtr(enumMoniker);
        releaseComPtr(devEnum);
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return names;
    }
    if (FAILED(hr)) {
        if (error != nullptr) {
            *error = QStringLiteral("CreateClassEnumerator(VideoInput) failed: %1").arg(formatHResult(hr));
        }
        releaseComPtr(enumMoniker);
        releaseComPtr(devEnum);
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return names;
    }

    while (true) {
        IMoniker* moniker = nullptr;
        ULONG fetched = 0;
        const HRESULT nextResult = enumMoniker->Next(1, &moniker, &fetched);
        if (nextResult != S_OK || moniker == nullptr) {
            releaseComPtr(moniker);
            break;
        }

        IPropertyBag* propertyBag = nullptr;
        hr = moniker->BindToStorage(nullptr,
                                    nullptr,
                                    IID_IPropertyBag,
                                    reinterpret_cast<void**>(&propertyBag));
        if (SUCCEEDED(hr) && propertyBag != nullptr) {
            VARIANT friendlyName;
            VariantInit(&friendlyName);
            if (SUCCEEDED(propertyBag->Read(L"FriendlyName", &friendlyName, nullptr)) &&
                friendlyName.vt == VT_BSTR &&
                friendlyName.bstrVal != nullptr) {
                appendUniqueDeviceName(names, QString::fromWCharArray(friendlyName.bstrVal));
            }
            VariantClear(&friendlyName);
        }

        releaseComPtr(propertyBag);
        releaseComPtr(moniker);
    }

    if (error != nullptr) {
        error->clear();
    }
    releaseComPtr(enumMoniker);
    releaseComPtr(devEnum);
    if (shouldUninitialize) {
        CoUninitialize();
    }
    return names;
}
#endif

QStringList availableDeviceNames() {
    QStringList names = qtAvailableDeviceNames();
#ifdef _WIN32
    QString fallbackError;
    const QStringList fallbackNames = dshowAvailableDeviceNames(&fallbackError);
    for (const QString& fallbackName : fallbackNames) {
        appendUniqueDeviceName(names, fallbackName);
    }
#endif
    return names;
}

QString firstFallbackDeviceSelection() {
#ifdef _WIN32
    const QStringList fallbackNames = dshowAvailableDeviceNames();
    if (!fallbackNames.isEmpty()) {
        return fallbackNames.front();
    }
#endif
    return {};
}

QString configuredDeviceSelection() {
    return normalizeDeviceSelection(qEnvironmentVariable("MEETING_CAMERA_DEVICE_NAME"));
}

QString configuredCaptureBackend() {
    return normalizeDeviceSelection(qEnvironmentVariable("MEETING_CAMERA_CAPTURE_BACKEND")).toLower();
}

bool preferDshowBackend() {
    const QString backend = configuredCaptureBackend();
    return backend == QStringLiteral("dshow");
}

bool preferFfmpegProcessBackend() {
    const QString backend = configuredCaptureBackend();
    return backend == QStringLiteral("ffmpeg") || backend == QStringLiteral("ffmpeg-process");
}

bool preferQtBackend() {
    const QString backend = configuredCaptureBackend();
    return backend == QStringLiteral("qt") || backend == QStringLiteral("qtmultimedia");
}

QCameraDevice chooseDevice(const QCameraDevice& requested, const QString& selection) {
    if (!requested.isNull()) {
        return requested;
    }

    const QString preferredDeviceName = normalizeDeviceSelection(selection);
    if (!preferredDeviceName.isEmpty()) {
        const QCameraDevice preferredDevice = findDeviceBySelection(preferredDeviceName);
        if (!preferredDevice.isNull()) {
            return preferredDevice;
        }
    }

    const auto inputs = QMediaDevices::videoInputs();
    const QCameraDevice defaultDevice = QMediaDevices::defaultVideoInput();
    if (!defaultDevice.isNull()) {
        return defaultDevice;
    }

    if (!inputs.isEmpty()) {
        return inputs.front();
    }

    return {};
}

QString ffmpegExecutable() {
    const QString configured = normalizeDeviceSelection(qEnvironmentVariable("MEETING_FFMPEG_EXE"));
    return configured.isEmpty() ? QStringLiteral("ffmpeg") : configured;
}

QString formatAvError(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

template <typename Fn>
auto invokeOnQtThread(Fn&& fn) -> decltype(fn()) {
    using ReturnT = decltype(fn());
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr || QThread::currentThread() == app->thread()) {
        return fn();
    }

    if constexpr (std::is_void_v<ReturnT>) {
        QMetaObject::invokeMethod(app, [func = std::forward<Fn>(fn)]() mutable { func(); }, Qt::BlockingQueuedConnection);
    } else {
        ReturnT result{};
        QMetaObject::invokeMethod(app, [&result, func = std::forward<Fn>(fn)]() mutable { result = func(); }, Qt::BlockingQueuedConnection);
        return result;
    }
}

struct AVFormatContextDeleter {
    void operator()(AVFormatContext* context) const {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

struct SwsContextDeleter {
    void operator()(SwsContext* context) const {
        if (context != nullptr) {
            sws_freeContext(context);
        }
    }
};

using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

}  // namespace

struct CameraCapture::Impl {
    using FrameCallback = CameraCapture::FrameCallback;
    static constexpr auto kFallbackStartupTimeout = std::chrono::seconds(5);

    explicit Impl(QCameraDevice device = {}, QString deviceSelection = {})
        : requestedDevice(std::move(device)),
          requestedDeviceSelection(normalizeDeviceSelection(deviceSelection)) {}

    struct DshowStartupState {
        std::mutex mutex;
        std::condition_variable cv;
        bool completed{false};
        bool success{false};
        QString error;
    };

    static QList<QCameraDevice> availableDevices() {
        return invokeOnQtThread([]() {
            return QMediaDevices::videoInputs();
        });
    }

    static QStringList availableDeviceNames() {
        return invokeOnQtThread([]() {
            return ::av::capture::availableDeviceNames();
        });
    }

    QString effectiveDeviceSelection() const {
        const QString explicitSelection = normalizeDeviceSelection(requestedDeviceSelection);
        if (!explicitSelection.isEmpty()) {
            return explicitSelection;
        }
        if (!requestedDevice.isNull()) {
            return deviceNameForSelection(requestedDevice);
        }

        const QString configuredSelection = configuredDeviceSelection();
        if (!configuredSelection.isEmpty()) {
            return configuredSelection;
        }

        return {};
    }

    QStringList fallbackDeviceSelections() const {
        QStringList selections;
        appendUniqueDeviceName(selections, normalizeDeviceSelection(requestedDeviceSelection));
        if (!requestedDevice.isNull()) {
            appendUniqueDeviceName(selections, deviceNameForSelection(requestedDevice));
        }
        appendUniqueDeviceName(selections, configuredDeviceSelection());

        if (!selections.isEmpty()) {
            return selections;
        }

        const QStringList discoveredSelections = ::av::capture::availableDeviceNames();
        for (const QString& discoveredSelection : discoveredSelections) {
            appendUniqueDeviceName(selections, discoveredSelection);
        }
        return selections;
    }

    QString lastError() const {
        std::lock_guard<std::mutex> lock(mutex);
        return captureError;
    }

    QString backendName() const {
        std::lock_guard<std::mutex> lock(mutex);
        return activeBackend;
    }

    void setLastError(QString message) {
        std::lock_guard<std::mutex> lock(mutex);
        captureError = message.trimmed();
    }

    void clearLastError() {
        std::lock_guard<std::mutex> lock(mutex);
        captureError.clear();
    }

    void setBackendName(const QString& backend) {
        std::lock_guard<std::mutex> lock(mutex);
        activeBackend = backend.trimmed();
    }

    void emitFrame(CameraCaptureFrame frame) {
        if (!running.load(std::memory_order_acquire) || !frame.isValid()) {
            return;
        }

        FrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = frameCallback;
        }

        if (callback) {
            callback(std::move(frame));
        }
    }

    void emitVideoFrame(const QVideoFrame& frame) {
        CameraCaptureFrame capturedFrame;
        capturedFrame.videoFrame = frame;
        emitFrame(std::move(capturedFrame));
    }

    void emitRawBgraFrame(std::vector<uint8_t> bytes, int frameWidth, int frameHeight, int frameStride) {
        CameraCaptureFrame capturedFrame;
        capturedFrame.bgra = std::move(bytes);
        capturedFrame.width = frameWidth;
        capturedFrame.height = frameHeight;
        capturedFrame.stride = frameStride;
        emitFrame(std::move(capturedFrame));
    }

    bool startQtCapture(const std::shared_ptr<Impl>& self, const QCameraDevice& device) {
        auto newCamera = std::make_unique<QCamera>(device);
        auto newSink = std::make_unique<QVideoSink>();

        session.setVideoOutput(newSink.get());
        session.setCamera(newCamera.get());

        QObject::connect(newSink.get(), &QVideoSink::videoFrameChanged, newSink.get(), [self](const QVideoFrame& frame) {
            if (!self) {
                return;
            }
            self->emitVideoFrame(frame);
        });
        QObject::connect(newCamera.get(), &QCamera::errorOccurred, newCamera.get(), [self](QCamera::Error error, const QString& errorString) {
            if (!self || error == QCamera::NoError) {
                return;
            }
            const QString detail = errorString.trimmed().isEmpty()
                ? QStringLiteral("qt camera error %1").arg(static_cast<int>(error))
                : errorString.trimmed();
            self->setLastError(detail);
            self->running.store(false, std::memory_order_release);
            qWarning().noquote() << "[camera-capture] qt backend error:" << detail;
        });

        camera = std::move(newCamera);
        videoSink = std::move(newSink);
        dshowFallbackActive.store(false, std::memory_order_release);
        running.store(true, std::memory_order_release);
        setBackendName(QStringLiteral("qt"));
        camera->start();
        clearLastError();
        return true;
    }

    bool openDshowCapture(const QString& deviceSelection) {
        const QString normalizedSelection = normalizeDeviceSelection(deviceSelection);
        if (normalizedSelection.isEmpty()) {
            setLastError(QStringLiteral("no Qt camera device and no explicit camera selection for dshow fallback"));
            return false;
        }

        static std::once_flag registerOnce;
        std::call_once(registerOnce, []() {
            avdevice_register_all();
        });

        const AVInputFormat* inputFormat = av_find_input_format("dshow");
        if (inputFormat == nullptr) {
            setLastError(QStringLiteral("ffmpeg dshow input unavailable"));
            return false;
        }

        AVFormatContext* rawContext = avformat_alloc_context();
        if (rawContext == nullptr) {
            setLastError(QStringLiteral("ffmpeg input context allocation failed"));
            return false;
        }

        rawContext->interrupt_callback.callback = [](void* opaque) -> int {
            auto* self = static_cast<Impl*>(opaque);
            return self != nullptr && self->stopRequested.load(std::memory_order_acquire) ? 1 : 0;
        };
        rawContext->interrupt_callback.opaque = this;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtbufsize", "256M", 0);

        const QByteArray inputName = QStringLiteral("video=%1").arg(normalizedSelection).toUtf8();
        const int openResult = avformat_open_input(&rawContext, inputName.constData(), inputFormat, &options);
        av_dict_free(&options);

        AVFormatContextPtr formatContext(rawContext);
        if (openResult < 0) {
            setLastError(QStringLiteral("dshow open failed: %1").arg(formatAvError(openResult)));
            return false;
        }

        const int streamInfoResult = avformat_find_stream_info(formatContext.get(), nullptr);
        if (streamInfoResult < 0) {
            setLastError(QStringLiteral("dshow stream info failed: %1").arg(formatAvError(streamInfoResult)));
            return false;
        }

        const int streamIndex = av_find_best_stream(formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0) {
            setLastError(QStringLiteral("dshow video stream discovery failed: %1").arg(formatAvError(streamIndex)));
            return false;
        }

        const AVStream* stream = formatContext->streams[streamIndex];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) {
            setLastError(QStringLiteral("dshow decoder unavailable for codec %1").arg(stream->codecpar->codec_id));
            return false;
        }

        av::AVCodecContextPtr codecContext(avcodec_alloc_context3(decoder));
        if (!codecContext) {
            setLastError(QStringLiteral("dshow decoder context allocation failed"));
            return false;
        }

        const int copyParamsResult = avcodec_parameters_to_context(codecContext.get(), stream->codecpar);
        if (copyParamsResult < 0) {
            setLastError(QStringLiteral("dshow decoder parameter copy failed: %1").arg(formatAvError(copyParamsResult)));
            return false;
        }

        const int openDecoderResult = avcodec_open2(codecContext.get(), decoder, nullptr);
        if (openDecoderResult < 0) {
            setLastError(QStringLiteral("dshow decoder open failed: %1").arg(formatAvError(openDecoderResult)));
            return false;
        }

        av::AVFramePtr frame = av::makeFrame();
        av::AVPacketPtr packet = av::makePacket();
        if (!frame || !packet) {
            setLastError(QStringLiteral("dshow frame/packet allocation failed"));
            return false;
        }

        dshowFormatContext = std::move(formatContext);
        dshowCodecContext = std::move(codecContext);
        dshowFrame = std::move(frame);
        dshowPacket = std::move(packet);
        dshowVideoStreamIndex = streamIndex;
        dshowSwsContext.reset();
        dshowFallbackActive.store(true, std::memory_order_release);
        setBackendName(QStringLiteral("dshow"));
        clearLastError();
        return true;
    }

    bool probeFfmpegProcessCapture(const QString& deviceSelection, QSize& frameSize, QString& error) {
        const QString normalizedSelection = normalizeDeviceSelection(deviceSelection);
        if (normalizedSelection.isEmpty()) {
            error = QStringLiteral("no explicit camera selection for ffmpeg-process fallback");
            return false;
        }

        QProcess process;
        process.setProgram(ffmpegExecutable());
        process.setArguments({
            QStringLiteral("-hide_banner"),
            QStringLiteral("-f"), QStringLiteral("dshow"),
            QStringLiteral("-i"), QStringLiteral("video=%1").arg(normalizedSelection),
            QStringLiteral("-frames:v"), QStringLiteral("1"),
            QStringLiteral("-f"), QStringLiteral("null"),
            QStringLiteral("-")
        });
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start();
        if (!process.waitForStarted(5000)) {
            error = QStringLiteral("ffmpeg probe start failed: %1").arg(process.errorString());
            return false;
        }
        process.waitForFinished(15000);

        const QString output = QString::fromUtf8(process.readAllStandardError()) +
                               QString::fromUtf8(process.readAllStandardOutput());
        const QRegularExpression sizePattern(QStringLiteral("(\\d{2,5})x(\\d{2,5})(?:,|\\s)"));
        const QRegularExpressionMatch match = sizePattern.match(output);
        if (!match.hasMatch()) {
            error = output.trimmed().isEmpty()
                ? QStringLiteral("ffmpeg probe failed without video size")
                : output.trimmed();
            return false;
        }

        bool widthOk = false;
        bool heightOk = false;
        const int width = match.captured(1).toInt(&widthOk);
        const int height = match.captured(2).toInt(&heightOk);
        if (!widthOk || !heightOk || width <= 0 || height <= 0) {
            error = QStringLiteral("ffmpeg probe returned invalid frame size");
            return false;
        }

        frameSize = QSize(width, height);
        return true;
    }

    bool convertDshowFrameToBgra(const AVFrame& frame,
                                 std::vector<uint8_t>& outputBytes,
                                 int& outputWidth,
                                 int& outputHeight,
                                 int& outputStride) {
        if (frame.width <= 0 || frame.height <= 0 || frame.format < 0) {
            return false;
        }

        const AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(frame.format);
        SwsContext* rawSwsContext = sws_getCachedContext(dshowSwsContext.release(),
                                                         frame.width,
                                                         frame.height,
                                                         sourceFormat,
                                                         frame.width,
                                                         frame.height,
                                                         AV_PIX_FMT_BGRA,
                                                         SWS_BILINEAR,
                                                         nullptr,
                                                         nullptr,
                                                         nullptr);
        if (rawSwsContext == nullptr) {
            return false;
        }
        dshowSwsContext.reset(rawSwsContext);

        outputWidth = frame.width;
        outputHeight = frame.height;
        outputStride = frame.width * 4;
        outputBytes.resize(static_cast<std::size_t>(outputStride) * static_cast<std::size_t>(outputHeight));
        uint8_t* destinationData[4] = {outputBytes.data(), nullptr, nullptr, nullptr};
        int destinationLinesize[4] = {outputStride, 0, 0, 0};
        if (sws_scale(dshowSwsContext.get(),
                      frame.data,
                      frame.linesize,
                      0,
                      frame.height,
                      destinationData,
                      destinationLinesize) <= 0) {
            outputBytes.clear();
            outputWidth = 0;
            outputHeight = 0;
            outputStride = 0;
            return false;
        }

        return true;
    }

    void runDshowLoop(const std::shared_ptr<Impl>& self,
                      const std::shared_ptr<DshowStartupState>& startupState,
                      const QString& deviceSelection) {
        const bool opened = openDshowCapture(deviceSelection);
        {
            std::lock_guard<std::mutex> lock(startupState->mutex);
            startupState->completed = true;
            startupState->success = opened;
            startupState->error = lastError();
        }
        startupState->cv.notify_one();

        if (!opened) {
            dshowFallbackActive.store(false, std::memory_order_release);
            running.store(false, std::memory_order_release);
            return;
        }

        QString failureReason;

        while (running.load(std::memory_order_acquire) &&
               !stopRequested.load(std::memory_order_acquire)) {
            const int readResult = av_read_frame(dshowFormatContext.get(), dshowPacket.get());
            if (readResult == AVERROR(EAGAIN)) {
                continue;
            }
            if (readResult == AVERROR_EXIT && stopRequested.load(std::memory_order_acquire)) {
                break;
            }
            if (readResult < 0) {
                if (!stopRequested.load(std::memory_order_acquire)) {
                    failureReason = QStringLiteral("dshow read failed: %1").arg(formatAvError(readResult));
                }
                break;
            }

            if (dshowPacket->stream_index != dshowVideoStreamIndex) {
                av_packet_unref(dshowPacket.get());
                continue;
            }

            const int sendResult = avcodec_send_packet(dshowCodecContext.get(), dshowPacket.get());
            av_packet_unref(dshowPacket.get());
            if (sendResult == AVERROR(EAGAIN)) {
                continue;
            }
            if (sendResult < 0) {
                if (!stopRequested.load(std::memory_order_acquire)) {
                    failureReason = QStringLiteral("dshow decode send failed: %1").arg(formatAvError(sendResult));
                }
                break;
            }

            while (running.load(std::memory_order_acquire) &&
                   !stopRequested.load(std::memory_order_acquire)) {
                const int receiveResult = avcodec_receive_frame(dshowCodecContext.get(), dshowFrame.get());
                if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                    break;
                }
                if (receiveResult < 0) {
                    if (!stopRequested.load(std::memory_order_acquire)) {
                        failureReason = QStringLiteral("dshow decode receive failed: %1").arg(formatAvError(receiveResult));
                    }
                    break;
                }

                std::vector<uint8_t> bgraBytes;
                int frameWidth = 0;
                int frameHeight = 0;
                int frameStride = 0;
                const bool converted = convertDshowFrameToBgra(*dshowFrame,
                                                               bgraBytes,
                                                               frameWidth,
                                                               frameHeight,
                                                               frameStride);
                av_frame_unref(dshowFrame.get());
                if (!converted) {
                    failureReason = QStringLiteral("dshow frame conversion failed");
                    break;
                }

                emitRawBgraFrame(std::move(bgraBytes), frameWidth, frameHeight, frameStride);
            }

            if (!failureReason.isEmpty()) {
                break;
            }
        }

        if (!failureReason.isEmpty()) {
            setLastError(failureReason);
            qWarning().noquote() << "[camera-capture] ffmpeg fallback failed:" << failureReason;
        }
        dshowFallbackActive.store(false, std::memory_order_release);
        if (!stopRequested.load(std::memory_order_acquire)) {
            running.store(false, std::memory_order_release);
        }
    }

    void runFfmpegProcessLoop(const std::shared_ptr<Impl>& self,
                              const std::shared_ptr<DshowStartupState>& startupState,
                              const QString& deviceSelection) {
        QSize frameSize;
        QString startupError;
        if (!probeFfmpegProcessCapture(deviceSelection, frameSize, startupError)) {
            {
                std::lock_guard<std::mutex> lock(startupState->mutex);
                startupState->completed = true;
                startupState->success = false;
                startupState->error = startupError;
            }
            startupState->cv.notify_one();
            dshowFallbackActive.store(false, std::memory_order_release);
            running.store(false, std::memory_order_release);
            return;
        }

        QProcess process;
        process.setProgram(ffmpegExecutable());
        process.setArguments({
            QStringLiteral("-hide_banner"),
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("dshow"),
            QStringLiteral("-i"), QStringLiteral("video=%1").arg(normalizeDeviceSelection(deviceSelection)),
            QStringLiteral("-an"),
            QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
            QStringLiteral("-f"), QStringLiteral("rawvideo"),
            QStringLiteral("pipe:1")
        });
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.start();
        if (!process.waitForStarted(5000)) {
            {
                std::lock_guard<std::mutex> lock(startupState->mutex);
                startupState->completed = true;
                startupState->success = false;
                startupState->error = QStringLiteral("ffmpeg stream start failed: %1").arg(process.errorString());
            }
            startupState->cv.notify_one();
            dshowFallbackActive.store(false, std::memory_order_release);
            running.store(false, std::memory_order_release);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(startupState->mutex);
            startupState->completed = true;
            startupState->success = true;
            startupState->error.clear();
        }
        startupState->cv.notify_one();

        const qsizetype frameBytes = static_cast<qsizetype>(frameSize.width()) *
                                     static_cast<qsizetype>(frameSize.height()) * 4;
        QByteArray outputBuffer;
        outputBuffer.reserve(frameBytes * 2);
        QString stderrText;

        while (running.load(std::memory_order_acquire) &&
               !stopRequested.load(std::memory_order_acquire)) {
            if (!process.waitForReadyRead(100)) {
                stderrText += QString::fromUtf8(process.readAllStandardError());
                if (process.state() == QProcess::NotRunning) {
                    break;
                }
                continue;
            }

            outputBuffer.append(process.readAllStandardOutput());
            stderrText += QString::fromUtf8(process.readAllStandardError());
            while (outputBuffer.size() >= frameBytes) {
                std::vector<uint8_t> bgraBytes(static_cast<std::size_t>(frameBytes));
                std::memcpy(bgraBytes.data(), outputBuffer.constData(), static_cast<std::size_t>(frameBytes));
                emitRawBgraFrame(std::move(bgraBytes),
                                 frameSize.width(),
                                 frameSize.height(),
                                 frameSize.width() * 4);
                outputBuffer.remove(0, frameBytes);
            }
        }

        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(2000);
        }
        stderrText += QString::fromUtf8(process.readAllStandardError());

        if (!stopRequested.load(std::memory_order_acquire) &&
            process.exitStatus() != QProcess::NormalExit &&
            stderrText.trimmed().isEmpty()) {
            stderrText = QStringLiteral("ffmpeg-process exited abnormally");
        }
        if (!stopRequested.load(std::memory_order_acquire) &&
            !stderrText.trimmed().isEmpty()) {
            setLastError(stderrText.trimmed());
            qWarning().noquote() << "[camera-capture] ffmpeg-process fallback failed:" << stderrText.trimmed();
        }

        dshowFallbackActive.store(false, std::memory_order_release);
        if (!stopRequested.load(std::memory_order_acquire)) {
            running.store(false, std::memory_order_release);
        }
    }

    bool startFfmpegProcessFallback(const std::shared_ptr<Impl>& self, const QString& deviceSelection) {
        const auto startupState = std::make_shared<DshowStartupState>();
        stopRequested.store(false, std::memory_order_release);
        dshowFallbackActive.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        setBackendName(QStringLiteral("ffmpeg-process"));
        fallbackThread = std::thread([self, startupState, deviceSelection]() {
            if (self) {
                self->runFfmpegProcessLoop(self, startupState, deviceSelection);
            }
        });

        std::unique_lock<std::mutex> lock(startupState->mutex);
        if (!startupState->cv.wait_for(lock, kFallbackStartupTimeout, [startupState]() {
                return startupState->completed;
            })) {
            lock.unlock();
            stopRequested.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
            if (fallbackThread.joinable()) {
                fallbackThread.join();
            }
            resetDshowCapture();
            setLastError(QStringLiteral("ffmpeg-process startup timed out for %1")
                             .arg(deviceSelection.isEmpty() ? QStringLiteral("<default>") : deviceSelection));
            return false;
        }
        if (!startupState->success) {
            lock.unlock();
            stopRequested.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
            if (fallbackThread.joinable()) {
                fallbackThread.join();
            }
            resetDshowCapture();
            setLastError(startupState->error);
            return false;
        }

        qInfo().noquote() << "[camera-capture] using ffmpeg-process dshow fallback for" << deviceSelection;
        return true;
    }

    bool startDshowFallback(const std::shared_ptr<Impl>& self, const QString& deviceSelection) {
        const auto startupState = std::make_shared<DshowStartupState>();
        stopRequested.store(false, std::memory_order_release);
        dshowFallbackActive.store(true, std::memory_order_release);
        running.store(true, std::memory_order_release);
        setBackendName(QStringLiteral("dshow"));
        fallbackThread = std::thread([self, startupState, deviceSelection]() {
            if (self) {
                self->runDshowLoop(self, startupState, deviceSelection);
            }
        });

        std::unique_lock<std::mutex> lock(startupState->mutex);
        if (!startupState->cv.wait_for(lock, kFallbackStartupTimeout, [startupState]() {
                return startupState->completed;
            })) {
            lock.unlock();
            stopRequested.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
            if (fallbackThread.joinable()) {
                fallbackThread.join();
            }
            resetDshowCapture();
            setLastError(QStringLiteral("dshow startup timed out for %1")
                             .arg(deviceSelection.isEmpty() ? QStringLiteral("<default>") : deviceSelection));
            return false;
        }
        if (!startupState->success) {
            lock.unlock();
            stopRequested.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
            if (fallbackThread.joinable()) {
                fallbackThread.join();
            }
            resetDshowCapture();
            setLastError(startupState->error);
            return false;
        }

        qInfo().noquote() << "[camera-capture] using ffmpeg dshow fallback for" << deviceSelection;
        return true;
    }

    void resetDshowCapture() {
        dshowSwsContext.reset();
        dshowPacket.reset();
        dshowFrame.reset();
        dshowCodecContext.reset();
        dshowFormatContext.reset();
        dshowVideoStreamIndex = -1;
        dshowFallbackActive.store(false, std::memory_order_release);
        setBackendName(QString());
    }

    bool start(const std::shared_ptr<Impl>& self) {
        if (running.load(std::memory_order_acquire)) {
            return true;
        }

        if (QCoreApplication::instance() == nullptr) {
            setLastError(QStringLiteral("no Qt application instance"));
            qWarning().noquote() << "[camera-capture] no Qt application instance";
            return false;
        }

        clearLastError();
        stopRequested.store(false, std::memory_order_release);

        const QString deviceSelection = effectiveDeviceSelection();
        const bool explicitDeviceSelection = !deviceSelection.trimmed().isEmpty();
        const auto tryQtCapture = [&]() {
            const QCameraDevice device = chooseDevice(requestedDevice, deviceSelection);
            if (!device.isNull()) {
                return startQtCapture(self, device);
            }
            setLastError(QStringLiteral("no available Qt camera device"));
            qWarning().noquote() << "[camera-capture] no available Qt camera device";
            return false;
        };

        if (preferQtBackend()) {
            return tryQtCapture();
        }

#ifdef _WIN32
        if (explicitDeviceSelection || preferDshowBackend() || preferFfmpegProcessBackend()) {
            const QStringList fallbackSelections = fallbackDeviceSelections();
            auto tryFallbackSelections = [&](bool ffmpegFirst) {
                QStringList attemptErrors;
                for (const QString& selection : fallbackSelections) {
                    const QString normalizedSelection = normalizeDeviceSelection(selection);
                    if (normalizedSelection.isEmpty()) {
                        continue;
                    }

                    if (ffmpegFirst) {
                        if (startFfmpegProcessFallback(self, normalizedSelection)) {
                            return true;
                        }
                        const QString ffmpegError = lastError();
                        if (startDshowFallback(self, normalizedSelection)) {
                            return true;
                        }
                        const QString dshowError = lastError();
                        attemptErrors.append(QStringLiteral("%1 => ffmpeg-process fallback failed: %2; dshow fallback failed: %3")
                                                 .arg(normalizedSelection,
                                                      ffmpegError.isEmpty() ? QStringLiteral("<none>") : ffmpegError,
                                                      dshowError.isEmpty() ? QStringLiteral("<none>") : dshowError));
                    } else {
                        if (startDshowFallback(self, normalizedSelection)) {
                            return true;
                        }
                        const QString dshowError = lastError();
                        if (startFfmpegProcessFallback(self, normalizedSelection)) {
                            return true;
                        }
                        const QString ffmpegError = lastError();
                        attemptErrors.append(QStringLiteral("%1 => dshow fallback failed: %2; ffmpeg-process fallback failed: %3")
                                                 .arg(normalizedSelection,
                                                      dshowError.isEmpty() ? QStringLiteral("<none>") : dshowError,
                                                      ffmpegError.isEmpty() ? QStringLiteral("<none>") : ffmpegError));
                    }
                }

                if (!attemptErrors.isEmpty()) {
                    setLastError(attemptErrors.join(QStringLiteral("; ")));
                }
                return false;
            };

            if (preferDshowBackend()) {
                if (tryFallbackSelections(false)) {
                    return true;
                }
            } else if (preferFfmpegProcessBackend()) {
                if (tryFallbackSelections(true)) {
                    return true;
                }
            } else {
                if (tryFallbackSelections(false)) {
                    return true;
                }
            }
            if (explicitDeviceSelection && !preferDshowBackend() && !preferFfmpegProcessBackend() && tryQtCapture()) {
                return true;
            }
            if (preferDshowBackend() || preferFfmpegProcessBackend()) {
                qWarning().noquote() << "[camera-capture] start failed:" << lastError();
                return false;
            }
        }
#endif

        if (tryQtCapture()) {
            return true;
        }

        if (lastError().isEmpty()) {
            setLastError(QStringLiteral("no available camera device"));
        }
        qWarning().noquote() << "[camera-capture] start failed:" << lastError();
        return false;
    }

    void stop() {
        if (!running.exchange(false, std::memory_order_acq_rel) &&
            !fallbackThread.joinable() &&
            !camera &&
            !videoSink &&
            !dshowFormatContext) {
            return;
        }

        stopRequested.store(true, std::memory_order_release);

        session.setVideoOutput(nullptr);
        session.setCamera(nullptr);

        if (camera) {
            camera->stop();
        }

        videoSink.reset();
        camera.reset();

        if (fallbackThread.joinable()) {
            fallbackThread.join();
        }

        resetDshowCapture();
    }

    void setFrameCallback(FrameCallback callback) {
        std::lock_guard<std::mutex> lock(mutex);
        frameCallback = std::move(callback);
    }

    bool setDevice(const QCameraDevice& device, const std::shared_ptr<Impl>& self) {
        const bool wasRunning = running.load(std::memory_order_acquire);
        const QCameraDevice previousDevice = requestedDevice;
        const QString previousSelection = requestedDeviceSelection;

        stop();
        requestedDevice = device;
        requestedDeviceSelection = device.isNull() ? QString() : deviceNameForSelection(device);

        if (wasRunning && !start(self)) {
            qWarning().noquote() << "[camera-capture] failed to restart after device switch";
            requestedDevice = previousDevice;
            requestedDeviceSelection = previousSelection;
            if (!start(self)) {
                qWarning().noquote() << "[camera-capture] failed to restore previous camera device";
            }
            return false;
        }

        return true;
    }

    bool setDeviceSelection(const QString& selection, const std::shared_ptr<Impl>& self) {
        const bool wasRunning = running.load(std::memory_order_acquire);
        const QCameraDevice previousDevice = requestedDevice;
        const QString previousSelection = requestedDeviceSelection;

        stop();
        requestedDevice = {};
        requestedDeviceSelection = normalizeDeviceSelection(selection);

        if (wasRunning && !start(self)) {
            qWarning().noquote() << "[camera-capture] failed to restart after device-selection switch";
            requestedDevice = previousDevice;
            requestedDeviceSelection = previousSelection;
            if (!start(self)) {
                qWarning().noquote() << "[camera-capture] failed to restore previous camera selection";
            }
            return false;
        }

        return true;
    }

    QCameraDevice requestedDevice;
    QString requestedDeviceSelection;
    std::unique_ptr<QCamera> camera;
    std::unique_ptr<QVideoSink> videoSink;
    QMediaCaptureSession session;
    mutable std::mutex mutex;
    FrameCallback frameCallback;
    QString captureError;
    QString activeBackend;
    std::thread fallbackThread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> dshowFallbackActive{false};
    AVFormatContextPtr dshowFormatContext{nullptr};
    av::AVCodecContextPtr dshowCodecContext{nullptr};
    av::AVFramePtr dshowFrame{nullptr};
    av::AVPacketPtr dshowPacket{nullptr};
    SwsContextPtr dshowSwsContext{nullptr};
    int dshowVideoStreamIndex{-1};
};

CameraCapture::CameraCapture()
    : m_impl(std::make_shared<Impl>()) {}

CameraCapture::CameraCapture(QCameraDevice device)
    : m_impl(std::make_shared<Impl>(std::move(device))) {}

CameraCapture::CameraCapture(const QString& deviceSelection)
    : m_impl(std::make_shared<Impl>(QCameraDevice(), deviceSelection)) {}

CameraCapture::~CameraCapture() {
    stop();
}

QList<QCameraDevice> CameraCapture::availableDevices() {
    return Impl::availableDevices();
}

QStringList CameraCapture::availableDeviceNames() {
    return Impl::availableDeviceNames();
}

QString CameraCapture::deviceName(const QCameraDevice& device) {
    return deviceNameForSelection(device);
}

QCameraDevice CameraCapture::findDevice(const QString& selection) {
    return findDeviceBySelection(selection);
}

bool CameraCapture::start() {
    if (!m_impl) {
        return false;
    }
    return invokeOnQtThread([this]() {
        return m_impl->start(m_impl);
    });
}

void CameraCapture::stop() {
    if (!m_impl) {
        return;
    }
    invokeOnQtThread([this]() {
        m_impl->stop();
    });
}

bool CameraCapture::isRunning() const {
    return m_impl && m_impl->running.load(std::memory_order_acquire);
}

bool CameraCapture::setDevice(const QCameraDevice& device) {
    if (!m_impl) {
        return false;
    }
    return invokeOnQtThread([this, device]() {
        return m_impl->setDevice(device, m_impl);
    });
}

bool CameraCapture::setDeviceSelection(const QString& selection) {
    if (!m_impl) {
        return false;
    }
    return invokeOnQtThread([this, selection]() {
        return m_impl->setDeviceSelection(selection, m_impl);
    });
}

void CameraCapture::setFrameCallback(FrameCallback callback) {
    if (m_impl) {
        m_impl->setFrameCallback(std::move(callback));
    }
}

QString CameraCapture::lastError() const {
    return m_impl ? m_impl->lastError() : QString();
}

bool CameraCapture::usingDshowFallback() const {
    return m_impl && m_impl->dshowFallbackActive.load(std::memory_order_acquire);
}

QString CameraCapture::backendName() const {
    return m_impl ? m_impl->backendName() : QString();
}

}  // namespace av::capture
