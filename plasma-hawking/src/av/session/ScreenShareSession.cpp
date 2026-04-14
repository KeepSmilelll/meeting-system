#include "ScreenShareSession.h"

#include "av/capture/CameraCapture.h"
#include "av/capture/ScreenCapture.h"

#include <QDebug>
#include <QImage>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QMediaDevices>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace av::session {
namespace {

struct NaluView {
    const uint8_t* data{nullptr};
    std::size_t size{0};
};

std::vector<NaluView> splitAnnexBNalus(const std::vector<uint8_t>& payload) {
    std::vector<NaluView> nalus;
    if (payload.empty()) {
        return nalus;
    }

    auto findStartCode = [&payload](std::size_t from) -> std::pair<std::size_t, std::size_t> {
        for (std::size_t i = from; i + 3 < payload.size(); ++i) {
            if (payload[i] == 0x00 && payload[i + 1] == 0x00) {
                if (payload[i + 2] == 0x01) {
                    return {i, 3};
                }
                if (i + 3 < payload.size() && payload[i + 2] == 0x00 && payload[i + 3] == 0x01) {
                    return {i, 4};
                }
            }
        }
        return {payload.size(), 0};
    };

    auto [start, prefix] = findStartCode(0);
    if (prefix == 0) {
        nalus.push_back({payload.data(), payload.size()});
        return nalus;
    }

    while (prefix != 0) {
        const std::size_t naluStart = start + prefix;
        auto [nextStart, nextPrefix] = findStartCode(naluStart);
        const std::size_t naluEnd = nextPrefix == 0 ? payload.size() : nextStart;
        if (naluEnd > naluStart) {
            nalus.push_back({payload.data() + naluStart, naluEnd - naluStart});
        }
        start = nextStart;
        prefix = nextPrefix;
    }

    return nalus;
}

std::vector<std::vector<uint8_t>> packetizeH264(const std::vector<uint8_t>& encodedFrame,
                                                std::size_t maxPayloadBytes) {
    std::vector<std::vector<uint8_t>> packets;
    const std::vector<NaluView> nalus = splitAnnexBNalus(encodedFrame);
    for (const NaluView& nalu : nalus) {
        if (nalu.data == nullptr || nalu.size == 0) {
            continue;
        }

        if (nalu.size <= maxPayloadBytes) {
            packets.emplace_back(nalu.data, nalu.data + nalu.size);
            continue;
        }

        const uint8_t nalHeader = nalu.data[0];
        const uint8_t fuIndicator = static_cast<uint8_t>((nalHeader & 0xE0) | 28);
        const uint8_t fuHeaderBase = static_cast<uint8_t>(nalHeader & 0x1F);
        std::size_t offset = 1;
        const std::size_t fragmentBytes = maxPayloadBytes > 2 ? maxPayloadBytes - 2 : 0;
        if (fragmentBytes == 0) {
            continue;
        }

        while (offset < nalu.size) {
            const std::size_t remaining = nalu.size - offset;
            const std::size_t chunk = std::min(fragmentBytes, remaining);
            std::vector<uint8_t> packet;
            packet.reserve(chunk + 2);
            packet.push_back(fuIndicator);

            uint8_t fuHeader = fuHeaderBase;
            if (offset == 1) {
                fuHeader |= 0x80;
            }
            if (offset + chunk >= nalu.size) {
                fuHeader |= 0x40;
            }
            packet.push_back(fuHeader);
            packet.insert(packet.end(), nalu.data + static_cast<std::ptrdiff_t>(offset), nalu.data + static_cast<std::ptrdiff_t>(offset + chunk));
            packets.push_back(std::move(packet));
            offset += chunk;
        }
    }
    return packets;
}

uint32_t makeSsrc() {
    uint32_t value = 0;
    while (value == 0) {
        value = static_cast<uint32_t>(std::random_device{}());
    }
    return value;
}

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool allowSyntheticCameraFallback() {
    return qEnvironmentVariableIntValue("MEETING_SYNTHETIC_CAMERA") != 0;
}

QString normalizeCameraDeviceName(const QString& name) {
    return name.trimmed();
}

QString cameraDeviceLabel(const QCameraDevice& device) {
    const QString description = normalizeCameraDeviceName(device.description());
    if (!description.isEmpty()) {
        return description;
    }
    return QString::fromUtf8(device.id()).trimmed();
}

QCameraDevice findCameraDeviceByName(const QString& name) {
    const QString normalized = normalizeCameraDeviceName(name);
    if (normalized.isEmpty()) {
        return {};
    }

    const auto devices = av::capture::CameraCapture::availableDevices();
    for (const auto& device : devices) {
        const QString label = cameraDeviceLabel(device);
        if (label.compare(normalized, Qt::CaseInsensitive) == 0 ||
            QString::fromUtf8(device.id()).contains(normalized, Qt::CaseInsensitive)) {
            return device;
        }
    }

    for (const auto& device : devices) {
        if (device.description().contains(normalized, Qt::CaseInsensitive) ||
            QString::fromUtf8(device.id()).contains(normalized, Qt::CaseInsensitive)) {
            return device;
        }
    }

    return {};
}

struct H264AccessUnitAssembler {
    uint32_t timestamp{0};
    bool hasTimestamp{false};
    std::vector<uint8_t> accessUnit;
    std::vector<uint8_t> fragmentedNalu;

    static void appendStartCode(std::vector<uint8_t>& target) {
        static constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
        target.insert(target.end(), std::begin(kStartCode), std::end(kStartCode));
    }

    static void appendCompleteNalu(std::vector<uint8_t>& target, const uint8_t* data, std::size_t size) {
        if (data == nullptr || size == 0) {
            return;
        }
        appendStartCode(target);
        target.insert(target.end(), data, data + static_cast<std::ptrdiff_t>(size));
    }

    bool consume(const media::RTPPacket& packet, av::codec::EncodedVideoFrame& outFrame) {
        if (!hasTimestamp || packet.header.timestamp != timestamp) {
            fragmentedNalu.clear();
            accessUnit.clear();
            timestamp = packet.header.timestamp;
            hasTimestamp = true;
        }

        if (packet.payload.empty()) {
            return false;
        }

        const uint8_t nalType = static_cast<uint8_t>(packet.payload[0] & 0x1F);
        if (nalType == 28) {
            if (packet.payload.size() < 2) {
                return false;
            }

            const uint8_t fuIndicator = packet.payload[0];
            const uint8_t fuHeader = packet.payload[1];
            const bool start = (fuHeader & 0x80) != 0;
            const bool end = (fuHeader & 0x40) != 0;
            const uint8_t reconstructedHeader = static_cast<uint8_t>((fuIndicator & 0xE0) | (fuHeader & 0x1F));

            if (start) {
                fragmentedNalu.clear();
                fragmentedNalu.push_back(reconstructedHeader);
            }
            if (fragmentedNalu.empty()) {
                return false;
            }

            fragmentedNalu.insert(fragmentedNalu.end(), packet.payload.begin() + 2, packet.payload.end());
            if (end) {
                appendCompleteNalu(accessUnit, fragmentedNalu.data(), fragmentedNalu.size());
                fragmentedNalu.clear();
            }
        } else {
            appendCompleteNalu(accessUnit, packet.payload.data(), packet.payload.size());
        }

        if (!packet.header.marker || accessUnit.empty()) {
            return false;
        }

        outFrame.payload = std::move(accessUnit);
        outFrame.pts = static_cast<int64_t>(packet.header.timestamp);
        outFrame.payloadType = packet.header.payloadType;
        accessUnit.clear();
        return !outFrame.payload.empty();
    }
};
QCameraDevice resolvePreferredCameraDeviceName(const QString& preferredDeviceName) {
    const QString normalized = preferredDeviceName.trimmed();
    if (normalized.isEmpty()) {
        return {};
    }

    const auto inputs = QMediaDevices::videoInputs();
    for (const auto& input : inputs) {
        if (input.description().compare(normalized, Qt::CaseInsensitive) == 0 ||
            QString::fromUtf8(input.id()).compare(normalized, Qt::CaseInsensitive) == 0) {
            return input;
        }
    }

    for (const auto& input : inputs) {
        if (input.description().contains(normalized, Qt::CaseInsensitive) ||
            QString::fromUtf8(input.id()).contains(normalized, Qt::CaseInsensitive)) {
            return input;
        }
    }

    return {};
}
}  // namespace

struct ScreenShareSession::CameraFrameRelay {
    explicit CameraFrameRelay(int targetWidth, int targetHeight, int targetFrameRate)
        : width(std::max(2, targetWidth & ~1)),
          height(std::max(2, targetHeight & ~1)),
          frameInterval(std::chrono::milliseconds(std::max(1, 1000 / std::max(1, targetFrameRate)))) {}

    uint64_t beginCapture() {
        const uint64_t nextGeneration = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            frames.clear();
            lastAcceptedAt = std::chrono::steady_clock::time_point{};
        }
        nextPts.store(0, std::memory_order_release);
        cv.notify_all();
        return nextGeneration;
    }

    void invalidate() {
        generation.fetch_add(1, std::memory_order_acq_rel);
        setCameraEnabled(false);
    }

    void setCameraEnabled(bool enabled) {
        cameraEnabled.store(enabled, std::memory_order_release);
        if (!enabled) {
            std::lock_guard<std::mutex> lock(mutex);
            frames.clear();
            lastAcceptedAt = std::chrono::steady_clock::time_point{};
        }
        cv.notify_all();
    }

    void setSharingEnabled(bool enabled) {
        sharingEnabled.store(enabled, std::memory_order_release);
        cv.notify_all();
    }

    bool popFrame(av::capture::ScreenFrame& outFrame, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, timeout, [this] {
                return !frames.empty() ||
                       !cameraEnabled.load(std::memory_order_acquire) ||
                       sharingEnabled.load(std::memory_order_acquire);
            })) {
            return false;
        }
        if (frames.empty()) {
            return false;
        }

        outFrame = std::move(frames.front());
        frames.pop_front();
        return true;
    }

    bool enqueueFrame(uint64_t expectedGeneration, const QVideoFrame& frame) {
        if (!frame.isValid() ||
            !cameraEnabled.load(std::memory_order_acquire) ||
            sharingEnabled.load(std::memory_order_acquire) ||
            generation.load(std::memory_order_acquire) != expectedGeneration) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!cameraEnabled.load(std::memory_order_acquire) ||
                sharingEnabled.load(std::memory_order_acquire) ||
                generation.load(std::memory_order_acquire) != expectedGeneration) {
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            if (lastAcceptedAt != std::chrono::steady_clock::time_point{} &&
                now - lastAcceptedAt < frameInterval) {
                return false;
            }
            lastAcceptedAt = now;
        }

        QVideoFrame cpuFrame(frame);
        QImage bgra = cpuFrame.toImage();
        if (bgra.isNull() && cpuFrame.map(QVideoFrame::ReadOnly)) {
            bgra = cpuFrame.toImage();
            cpuFrame.unmap();
        }
        if (bgra.isNull()) {
            return false;
        }
        if (bgra.format() != QImage::Format_ARGB32) {
            bgra = bgra.convertToFormat(QImage::Format_ARGB32);
        }
        if (bgra.isNull()) {
            return false;
        }
        if (bgra.width() != width || bgra.height() != height) {
            bgra = bgra.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (bgra.format() != QImage::Format_ARGB32) {
                bgra = bgra.convertToFormat(QImage::Format_ARGB32);
            }
        }
        if (bgra.isNull()) {
            return false;
        }

        av::capture::ScreenFrame converted;
        converted.width = width;
        converted.height = height;
        converted.pts = nextPts.fetch_add(1, std::memory_order_acq_rel);

        const std::size_t lineBytes = static_cast<std::size_t>(width) * 4U;
        converted.bgra.resize(lineBytes * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y) {
            std::memcpy(converted.bgra.data() + static_cast<std::size_t>(y) * lineBytes,
                        bgra.constScanLine(y),
                        lineBytes);
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!cameraEnabled.load(std::memory_order_acquire) ||
                sharingEnabled.load(std::memory_order_acquire) ||
                generation.load(std::memory_order_acquire) != expectedGeneration) {
                return false;
            }
            if (frames.size() >= capacity) {
                frames.pop_front();
            }
            frames.push_back(std::move(converted));
        }
        cv.notify_one();
        return true;
    }

    const int width;
    const int height;
    const std::chrono::milliseconds frameInterval;
    static constexpr std::size_t capacity = 3U;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<av::capture::ScreenFrame> frames;
    std::chrono::steady_clock::time_point lastAcceptedAt;
    std::atomic<bool> cameraEnabled{false};
    std::atomic<bool> sharingEnabled{false};
    std::atomic<uint64_t> generation{0};
    std::atomic<int64_t> nextPts{0};
};

ScreenShareSession::ScreenShareSession(ScreenShareSessionConfig config)
    : m_config(std::move(config)),
      m_cameraRelay(std::make_shared<CameraFrameRelay>(m_config.width, m_config.height, m_config.frameRate)),
      m_sender(0, 0) {
    m_targetBitrateBps.store(static_cast<uint32_t>(std::max(1000, m_config.bitrate)), std::memory_order_release);
    m_appliedBitrateBps.store(static_cast<uint32_t>(std::max(1000, m_config.bitrate)), std::memory_order_release);
    m_targetBitrateUpdatedAtMs.store(steadyNowMs(), std::memory_order_release);
}

ScreenShareSession::~ScreenShareSession() {
    stop();
}

bool ScreenShareSession::start() {
#ifdef _WIN32
    if (m_running.load(std::memory_order_acquire)) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError.clear();
        if (!openSocketLocked()) {
            return false;
        }
    }

    m_running.store(true, std::memory_order_release);
    m_recvThread = std::thread(&ScreenShareSession::recvLoop, this);
    return true;
#else
    return false;
#endif
}

void ScreenShareSession::stop() {
#ifdef _WIN32
    if (!m_running.exchange(false, std::memory_order_acq_rel) && !m_recvThread.joinable() && !m_sendThread.joinable()) {
        return;
    }

    m_sharingEnabled.store(false, std::memory_order_release);
    m_cameraSendingEnabled.store(false, std::memory_order_release);
    m_forceKeyFramePending.store(false, std::memory_order_release);
    m_expectedRemoteVideoSsrc.store(0U, std::memory_order_release);
    if (m_cameraRelay) {
        m_cameraRelay->setSharingEnabled(false);
        m_cameraRelay->invalidate();
    }

    if (m_sendThread.joinable()) {
        m_sendThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stopCaptureLocked();
        stopCameraCaptureLocked();
        m_sender.setSSRC(0);
        m_sentPacketCache.clear();
        closeSocketLocked();
    }
    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }
#endif
}

bool ScreenShareSession::setSharingEnabled(bool enabled) {
#ifdef _WIN32
    if (enabled) {
        if (!m_running.load(std::memory_order_acquire) && !start()) {
            return false;
        }
        if (m_sharingEnabled.load(std::memory_order_acquire)) {
            return true;
        }

        bool startSendThread = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!startCaptureLocked()) {
                return false;
            }
            if (!m_cameraSendingEnabled.load(std::memory_order_acquire)) {
                m_sender.setSSRC(makeSsrc());
                m_sender.setSequence(0);
                m_sentPacketCache.clear();
                startSendThread = true;
            }
        }

        m_sharingEnabled.store(true, std::memory_order_release);
        if (m_cameraRelay) {
            m_cameraRelay->setSharingEnabled(true);
        }
        m_forceKeyFramePending.store(true, std::memory_order_release);
        if (startSendThread) {
            m_keyframeRequestCount.store(0, std::memory_order_release);
            m_retransmitPacketCount.store(0, std::memory_order_release);
            m_bitrateReconfigureCount.store(0, std::memory_order_release);
            m_targetBitrateBps.store(static_cast<uint32_t>(m_config.bitrate), std::memory_order_release);
            m_appliedBitrateBps.store(static_cast<uint32_t>(m_config.bitrate), std::memory_order_release);
            m_lastBitrateApplyDelayMs.store(0, std::memory_order_release);
            m_targetBitrateUpdatedAtMs.store(steadyNowMs(), std::memory_order_release);
            if (m_sendThread.joinable()) {
                m_sendThread.join();
            }
            m_sendThread = std::thread(&ScreenShareSession::sendLoop, this);
        }
        qInfo().noquote() << "[screen-session] sharing enabled localPort=" << localPort()
                          << "ssrc=" << videoSsrc();
        return true;
    }

    if (!m_sharingEnabled.exchange(false, std::memory_order_acq_rel)) {
        return true;
    }
    if (m_cameraRelay) {
        m_cameraRelay->setSharingEnabled(false);
    }

    const bool keepSending = m_cameraSendingEnabled.load(std::memory_order_acquire);
    if (!keepSending && m_sendThread.joinable()) {
        m_sendThread.join();
    }
    if (keepSending) {
        m_forceKeyFramePending.store(true, std::memory_order_release);
    } else {
        m_forceKeyFramePending.store(false, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stopCaptureLocked();
    }
    return true;
#else
    return false;
#endif
}

bool ScreenShareSession::sharingEnabled() const {
    return m_sharingEnabled.load(std::memory_order_acquire);
}

bool ScreenShareSession::setCameraSendingEnabled(bool enabled) {
#ifdef _WIN32
    if (enabled) {
        if (!m_running.load(std::memory_order_acquire) && !start()) {
            return false;
        }
        if (m_cameraSendingEnabled.load(std::memory_order_acquire)) {
            return true;
        }

        bool startSendThread = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!startCameraCaptureLocked()) {
                return false;
            }
            if (!m_sharingEnabled.load(std::memory_order_acquire)) {
                m_sender.setSSRC(makeSsrc());
                m_sender.setSequence(0);
                m_sentPacketCache.clear();
                startSendThread = true;
            }
        }

        m_cameraSendingEnabled.store(true, std::memory_order_release);
        if (m_cameraRelay) {
            m_cameraRelay->setCameraEnabled(true);
            m_cameraRelay->setSharingEnabled(m_sharingEnabled.load(std::memory_order_acquire));
        }
        if (!m_sharingEnabled.load(std::memory_order_acquire)) {
            m_forceKeyFramePending.store(true, std::memory_order_release);
        }
        if (startSendThread) {
            m_keyframeRequestCount.store(0, std::memory_order_release);
            m_retransmitPacketCount.store(0, std::memory_order_release);
            m_bitrateReconfigureCount.store(0, std::memory_order_release);
            m_targetBitrateBps.store(static_cast<uint32_t>(m_config.bitrate), std::memory_order_release);
            m_appliedBitrateBps.store(static_cast<uint32_t>(m_config.bitrate), std::memory_order_release);
            m_lastBitrateApplyDelayMs.store(0, std::memory_order_release);
            m_targetBitrateUpdatedAtMs.store(steadyNowMs(), std::memory_order_release);
            if (m_sendThread.joinable()) {
                m_sendThread.join();
            }
            m_sendThread = std::thread(&ScreenShareSession::sendLoop, this);
        }
        qInfo().noquote() << "[screen-session] camera sending enabled localPort=" << localPort()
                          << "ssrc=" << videoSsrc();
        return true;
    }

    if (!m_cameraSendingEnabled.exchange(false, std::memory_order_acq_rel)) {
        return true;
    }
    if (m_cameraRelay) {
        m_cameraRelay->setCameraEnabled(false);
    }

    const bool keepSending = m_sharingEnabled.load(std::memory_order_acquire);
    if (!keepSending && m_sendThread.joinable()) {
        m_sendThread.join();
    }
    if (!keepSending) {
        m_forceKeyFramePending.store(false, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stopCameraCaptureLocked();
    }
    return true;
#else
    return false;
#endif
}

bool ScreenShareSession::cameraSendingEnabled() const {
    return m_cameraSendingEnabled.load(std::memory_order_acquire);
}

bool ScreenShareSession::setPreferredCameraDeviceName(const std::string& deviceName) {
    const QString normalizedDeviceName = normalizeCameraDeviceName(QString::fromStdString(deviceName));
#ifdef _WIN32
    std::function<void(std::string)> statusCallback;
    QString statusMessage;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string requestedDeviceName = normalizedDeviceName.toStdString();
        if (m_preferredCameraDeviceName == requestedDeviceName) {
            return true;
        }

        m_preferredCameraDeviceName = requestedDeviceName;
        statusCallback = m_statusCallback;
        if (!m_cameraCapture || !m_cameraCapture->isRunning()) {
            return true;
        }

        const QCameraDevice requestedDevice = resolvePreferredCameraDeviceName(normalizedDeviceName);
        if (!m_cameraCapture->setDevice(requestedDevice)) {
            setErrorLocked("camera device switch failed");
            return false;
        }

        if (!requestedDevice.isNull()) {
            statusMessage = QStringLiteral("Video camera switched to %1").arg(cameraDeviceLabel(requestedDevice));
        } else if (normalizedDeviceName.isEmpty()) {
            statusMessage = QStringLiteral("Video camera switched to system default");
        } else {
            statusMessage = QStringLiteral("Preferred camera unavailable, using system default");
        }
    }

    if (statusCallback && !statusMessage.isEmpty()) {
        statusCallback(statusMessage.toStdString());
    }
    return true;
#else
    m_preferredCameraDeviceName = normalizedDeviceName.toStdString();
    return true;
#endif
}

std::string ScreenShareSession::preferredCameraDeviceName() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_preferredCameraDeviceName;
}

void ScreenShareSession::setExpectedRemoteVideoSsrc(uint32_t ssrc) {
    m_expectedRemoteVideoSsrc.store(ssrc, std::memory_order_release);
}

uint32_t ScreenShareSession::expectedRemoteVideoSsrc() const {
    return m_expectedRemoteVideoSsrc.load(std::memory_order_acquire);
}

void ScreenShareSession::setPeer(const std::string& address, uint16_t port) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.peerAddress = address;
    m_config.peerPort = port;
    const auto statusCallback = m_statusCallback;
    m_peerValid = false;
    if (m_socket != INVALID_SOCKET) {
        sockaddr_in peer{};
        if (resolvePeerLocked(peer)) {
            m_peer = peer;
            m_peerValid = true;
            qInfo().noquote() << "[screen-session] peer=" << QString::fromStdString(address) << ":" << port;
            if (statusCallback) {
                statusCallback("Video peer configured");
            }
        }
    }
#else
    (void)address;
    (void)port;
#endif
}

void ScreenShareSession::setDecodedFrameCallback(std::function<void(av::codec::DecodedVideoFrame)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_decodedFrameCallback = std::move(callback);
}

void ScreenShareSession::setErrorCallback(std::function<void(std::string)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_errorCallback = std::move(callback);
}

void ScreenShareSession::setCameraSourceCallback(std::function<void(bool syntheticFallback)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cameraSourceCallback = std::move(callback);
}

void ScreenShareSession::setStatusCallback(std::function<void(std::string)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_statusCallback = std::move(callback);
}

uint16_t ScreenShareSession::localPort() const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_socket == INVALID_SOCKET) {
        return 0;
    }
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
#else
    return 0;
#endif
}

uint32_t ScreenShareSession::videoSsrc() const {
    return m_sender.ssrc();
}

bool ScreenShareSession::isRunning() const {
    return m_running.load(std::memory_order_acquire);
}

std::string ScreenShareSession::lastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

uint64_t ScreenShareSession::sentPacketCount() const {
    return m_sentPacketCount.load(std::memory_order_acquire);
}

uint64_t ScreenShareSession::receivedPacketCount() const {
    return m_receivedPacketCount.load(std::memory_order_acquire);
}

uint64_t ScreenShareSession::keyframeRequestCount() const {
    return m_keyframeRequestCount.load(std::memory_order_acquire);
}

uint64_t ScreenShareSession::retransmitPacketCount() const {
    return m_retransmitPacketCount.load(std::memory_order_acquire);
}

uint64_t ScreenShareSession::bitrateReconfigureCount() const {
    return m_bitrateReconfigureCount.load(std::memory_order_acquire);
}

uint32_t ScreenShareSession::lastBitrateApplyDelayMs() const {
    return m_lastBitrateApplyDelayMs.load(std::memory_order_acquire);
}

uint32_t ScreenShareSession::targetBitrateBps() const {
    return m_targetBitrateBps.load(std::memory_order_acquire);
}

uint32_t ScreenShareSession::appliedBitrateBps() const {
    return m_appliedBitrateBps.load(std::memory_order_acquire);
}

#ifdef _WIN32
bool ScreenShareSession::openSocketLocked() {
    static std::once_flag wsaInitOnce;
    static int wsaInitStatus = 0;
    std::call_once(wsaInitOnce, [] {
        WSADATA data{};
        wsaInitStatus = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (wsaInitStatus != 0) {
        setErrorLocked("WSAStartup failed");
        return false;
    }

    m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        setErrorLocked("socket() failed");
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(m_config.localPort);
    if (m_config.localAddress.empty() || m_config.localAddress == "0.0.0.0") {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (InetPtonA(AF_INET, m_config.localAddress.c_str(), &local.sin_addr) != 1) {
        setErrorLocked("invalid local address");
        closeSocketLocked();
        return false;
    }

    if (::bind(m_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        setErrorLocked("bind() failed");
        closeSocketLocked();
        return false;
    }

    sockaddr_in peer{};
    if (resolvePeerLocked(peer)) {
        m_peer = peer;
        m_peerValid = true;
    }

    DWORD timeoutMs = 50;
    ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    return true;
}

void ScreenShareSession::closeSocketLocked() {
    if (m_socket != INVALID_SOCKET) {
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_peerValid = false;
}

bool ScreenShareSession::resolvePeerLocked(sockaddr_in& outPeer) const {
    if (m_config.peerAddress.empty() || m_config.peerPort == 0) {
        return false;
    }

    std::memset(&outPeer, 0, sizeof(outPeer));
    outPeer.sin_family = AF_INET;
    outPeer.sin_port = htons(m_config.peerPort);
    if (InetPtonA(AF_INET, m_config.peerAddress.c_str(), &outPeer.sin_addr) != 1) {
        return false;
    }
    return true;
}

bool ScreenShareSession::looksLikeRtcp(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 4) {
        return false;
    }

    const uint8_t version = static_cast<uint8_t>((data[0] >> 6) & 0x03);
    if (version != 2) {
        return false;
    }

    const uint8_t packetType = data[1];
    return packetType >= 192U && packetType <= 223U;
}

uint16_t ScreenShareSession::parseSequenceNumber(const std::vector<uint8_t>& packetBytes) {
    if (packetBytes.size() < media::kRtpMinHeaderSize) {
        return 0;
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(packetBytes[2]) << 8) |
                                 static_cast<uint16_t>(packetBytes[3]));
}

void ScreenShareSession::cacheSentPacketLocked(uint16_t sequenceNumber, std::vector<uint8_t> packetBytes) {
    if (packetBytes.empty()) {
        return;
    }
    m_sentPacketCache.emplace_back(sequenceNumber, std::move(packetBytes));
    while (m_sentPacketCache.size() > kRetransmitCacheLimit) {
        m_sentPacketCache.pop_front();
    }
}

bool ScreenShareSession::retransmitPacketLocked(uint16_t sequenceNumber) {
    if (m_socket == INVALID_SOCKET || !m_peerValid) {
        return false;
    }

    for (auto it = m_sentPacketCache.rbegin(); it != m_sentPacketCache.rend(); ++it) {
        if (it->first != sequenceNumber) {
            continue;
        }

        const std::vector<uint8_t>& packetBytes = it->second;
        const int sent = ::sendto(m_socket,
                                  reinterpret_cast<const char*>(packetBytes.data()),
                                  static_cast<int>(packetBytes.size()),
                                  0,
                                  reinterpret_cast<const sockaddr*>(&m_peer),
                                  sizeof(m_peer));
        if (sent == static_cast<int>(packetBytes.size())) {
            return true;
        }
        setErrorLocked("retransmit sendto failed");
        return false;
    }
    return false;
}

bool ScreenShareSession::handleRtcpFeedbackLocked(const uint8_t* data, std::size_t len) {
    std::vector<media::RTCPPacketSlice> slices;
    if (!m_rtcpHandler.parseCompoundPacket(data, len, slices)) {
        return false;
    }

    bool handled = false;
    const uint32_t localSsrc = m_sender.ssrc();
    for (const auto& slice : slices) {
        if (!m_rtcpHandler.isFeedbackPacket(slice.header)) {
            continue;
        }

        if (slice.header.packetType == 205U && slice.header.countOrFormat == 1U) {
            media::RTCPNackFeedback nack{};
            if (!m_rtcpHandler.parseNackFeedback(data + slice.offset, slice.size, nack)) {
                continue;
            }
            if (localSsrc == 0U || (nack.mediaSsrc != 0U && nack.mediaSsrc != localSsrc)) {
                continue;
            }

            bool retransmitted = false;
            for (const uint16_t sequenceNumber : nack.lostSequences) {
                if (!retransmitPacketLocked(sequenceNumber)) {
                    continue;
                }
                m_retransmitPacketCount.fetch_add(1, std::memory_order_acq_rel);
                retransmitted = true;
            }
            handled = handled || retransmitted;
            continue;
        }

        if (slice.header.packetType == 206U && slice.header.countOrFormat == 1U) {
            media::RTCPPliFeedback pli{};
            if (!m_rtcpHandler.parsePliFeedback(data + slice.offset, slice.size, pli)) {
                continue;
            }
            if (localSsrc != 0U && (pli.mediaSsrc == 0U || pli.mediaSsrc == localSsrc)) {
                m_forceKeyFramePending.store(true, std::memory_order_release);
                m_keyframeRequestCount.fetch_add(1, std::memory_order_acq_rel);
                handled = true;
            }
            continue;
        }

        if (slice.header.packetType == 206U && slice.header.countOrFormat == 15U) {
            media::RTCPRembFeedback remb{};
            if (!m_rtcpHandler.parseRembFeedback(data + slice.offset, slice.size, remb)) {
                continue;
            }

            const auto matched = std::find(remb.targetSsrcs.begin(), remb.targetSsrcs.end(), localSsrc);
            if (localSsrc == 0U || matched == remb.targetSsrcs.end()) {
                continue;
            }

            const uint32_t nextBitrate = std::max<uint32_t>(100000U, std::min<uint32_t>(8000000U, remb.bitrateBps));
            const uint32_t previousTarget = m_targetBitrateBps.exchange(nextBitrate, std::memory_order_acq_rel);
            if (previousTarget != nextBitrate) {
                m_targetBitrateUpdatedAtMs.store(steadyNowMs(), std::memory_order_release);
            }
            handled = true;
        }
    }

    return handled;
}

void ScreenShareSession::setErrorLocked(std::string message) {
    m_lastError = std::move(message);
    qWarning().noquote() << "[screen-session]" << QString::fromStdString(m_lastError);
}

bool ScreenShareSession::startCaptureLocked() {
    if (m_capture && m_capture->isRunning()) {
        return true;
    }

    av::capture::ScreenCaptureConfig captureConfig{};
    captureConfig.targetWidth = m_config.width;
    captureConfig.targetHeight = m_config.height;
    captureConfig.frameRate = m_config.frameRate;
    captureConfig.ringCapacity = 3;

    auto capture = std::make_shared<av::capture::ScreenCapture>(captureConfig);
    if (!capture->start()) {
        setErrorLocked("screen capture start failed");
        return false;
    }

    m_capture = std::move(capture);
    return true;
}

void ScreenShareSession::stopCaptureLocked() {
    if (m_capture) {
        m_capture->stop();
        m_capture.reset();
    }
}

bool ScreenShareSession::startCameraCaptureLocked() {
    if (!m_cameraRelay) {
        m_cameraRelay = std::make_shared<CameraFrameRelay>(m_config.width, m_config.height, m_config.frameRate);
    }
    if (m_cameraCapture && m_cameraCapture->isRunning()) {
        return true;
    }
    if (m_cameraFallbackCapture && m_cameraFallbackCapture->isRunning()) {
        return true;
    }

    const auto startSyntheticFallback = [this]() {
        av::capture::ScreenCaptureConfig fallbackConfig{};
        fallbackConfig.targetWidth = m_config.width;
        fallbackConfig.targetHeight = m_config.height;
        fallbackConfig.frameRate = m_config.frameRate;
        fallbackConfig.ringCapacity = 3;
        auto fallbackCapture = std::make_shared<av::capture::ScreenCapture>(fallbackConfig);
        if (!fallbackCapture->start()) {
            setErrorLocked("camera fallback capture start failed");
            return false;
        }

        m_cameraCapture.reset();
        m_cameraFallbackCapture = std::move(fallbackCapture);
        qInfo().noquote() << "[screen-session] camera fallback synthetic source enabled";
        if (m_cameraSourceCallback) {
            m_cameraSourceCallback(true);
        }
        return true;
    };

    if (allowSyntheticCameraFallback()) {
        m_cameraRelay->invalidate();
        return startSyntheticFallback();
    }

    const uint64_t generation = m_cameraRelay->beginCapture();
    const QString preferredCameraDevice = QString::fromStdString(m_preferredCameraDeviceName);
    const QCameraDevice requestedDevice = resolvePreferredCameraDeviceName(preferredCameraDevice);
    auto cameraCapture = requestedDevice.isNull()
                             ? std::make_unique<av::capture::CameraCapture>()
                             : std::make_unique<av::capture::CameraCapture>(requestedDevice);
    std::weak_ptr<CameraFrameRelay> weakRelay = m_cameraRelay;
    const auto statusCallback = m_statusCallback;
    auto firstCameraFrameObserved = std::make_shared<std::atomic<bool>>(false);
    cameraCapture->setFrameCallback([weakRelay, generation, statusCallback, firstCameraFrameObserved](QVideoFrame frame) {
        if (const auto relay = weakRelay.lock()) {
            if (relay->enqueueFrame(generation, frame) &&
                statusCallback &&
                !firstCameraFrameObserved->exchange(true, std::memory_order_acq_rel)) {
                statusCallback("Video camera frame observed");
            }
        }
    });
    if (cameraCapture->start()) {
        m_cameraFallbackCapture.reset();
        m_cameraCapture = std::move(cameraCapture);
        if (m_cameraSourceCallback) {
            m_cameraSourceCallback(false);
        }
        if (m_statusCallback) {
            if (!requestedDevice.isNull()) {
                m_statusCallback(QStringLiteral("Video camera device: %1").arg(requestedDevice.description()).toStdString());
            } else if (!preferredCameraDevice.trimmed().isEmpty()) {
                m_statusCallback(QStringLiteral("Preferred camera unavailable, using system default").toStdString());
            }
        }
        return true;
    }

    cameraCapture->setFrameCallback({});
    m_cameraRelay->invalidate();
    setErrorLocked("camera capture start failed");
    return false;
}

void ScreenShareSession::stopCameraFallbackCaptureLocked() {
    if (m_cameraFallbackCapture) {
        m_cameraFallbackCapture->stop();
        m_cameraFallbackCapture.reset();
    }
}

void ScreenShareSession::stopCameraCaptureLocked() {
    if (m_cameraRelay) {
        m_cameraRelay->invalidate();
    }
    if (m_cameraCapture) {
        m_cameraCapture->setFrameCallback({});
        m_cameraCapture->stop();
        m_cameraCapture.reset();
    }
    stopCameraFallbackCaptureLocked();
}

bool ScreenShareSession::shouldAcceptSenderLocked(const sockaddr_in& from) const {
    if (!m_peerValid) {
        return true;
    }

    return from.sin_family == AF_INET &&
           from.sin_port == m_peer.sin_port &&
           from.sin_addr.s_addr == m_peer.sin_addr.s_addr;
}

void ScreenShareSession::sendLoop() {
    av::codec::VideoEncoder encoder;
    if (!encoder.configure(m_config.width, m_config.height, m_config.frameRate, m_config.bitrate)) {
        std::function<void(std::string)> errorCallback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            setErrorLocked("video encoder configure failed");
            errorCallback = m_errorCallback;
        }
        if (errorCallback) {
            errorCallback("video encoder configure failed");
        }
        return;
    }
    m_appliedBitrateBps.store(static_cast<uint32_t>(encoder.bitrate()), std::memory_order_release);

    bool loggedFirstPacket = false;
    bool loggedFirstEncodedPacket = false;
    bool loggedFirstEncodePending = false;
    bool loggedFirstEncodeError = false;
    const auto statusCallback = [this]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_statusCallback;
    }();
    while (m_running.load(std::memory_order_acquire) &&
           (m_sharingEnabled.load(std::memory_order_acquire) ||
            m_cameraSendingEnabled.load(std::memory_order_acquire))) {
        const bool useScreenSource = m_sharingEnabled.load(std::memory_order_acquire);
        const bool useCameraSource = !useScreenSource && m_cameraSendingEnabled.load(std::memory_order_acquire);

        std::shared_ptr<av::capture::ScreenCapture> capture;
        std::shared_ptr<av::capture::ScreenCapture> cameraFallbackCapture;
        std::shared_ptr<CameraFrameRelay> cameraRelay;
        sockaddr_in peer{};
        bool peerReady = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (useScreenSource) {
                capture = m_capture;
            } else if (useCameraSource) {
                if (m_cameraCapture && m_cameraCapture->isRunning()) {
                    cameraRelay = m_cameraRelay;
                } else if (m_cameraFallbackCapture && m_cameraFallbackCapture->isRunning()) {
                    cameraFallbackCapture = m_cameraFallbackCapture;
                }
            }
            peerReady = m_peerValid;
            if (peerReady) {
                peer = m_peer;
            }
        }

        av::capture::ScreenFrame frame;
        if (useScreenSource) {
            if (!capture || !capture->popFrameForEncode(frame, std::chrono::milliseconds(100))) {
                continue;
            }
            if (!m_sharingEnabled.load(std::memory_order_acquire)) {
                continue;
            }
        } else if (useCameraSource) {
            if (cameraRelay) {
                if (!cameraRelay->popFrame(frame, std::chrono::milliseconds(100))) {
                    continue;
                }
            } else if (cameraFallbackCapture) {
                if (!cameraFallbackCapture->popFrameForEncode(frame, std::chrono::milliseconds(100))) {
                    continue;
                }
            } else {
                break;
            }
            if (m_sharingEnabled.load(std::memory_order_acquire) ||
                !m_cameraSendingEnabled.load(std::memory_order_acquire)) {
                continue;
            }
        } else {
            break;
        }

        if (!peerReady) {
            continue;
        }

        const uint32_t targetBitrate = m_targetBitrateBps.load(std::memory_order_acquire);
        if (targetBitrate >= 100000U && static_cast<int>(targetBitrate) != encoder.bitrate()) {
            if (!encoder.configure(m_config.width, m_config.height, m_config.frameRate, static_cast<int>(targetBitrate))) {
                std::lock_guard<std::mutex> lock(m_mutex);
                setErrorLocked("video encoder reconfigure failed");
                continue;
            }
            m_appliedBitrateBps.store(static_cast<uint32_t>(encoder.bitrate()), std::memory_order_release);
            m_bitrateReconfigureCount.fetch_add(1, std::memory_order_acq_rel);
            const uint64_t updatedAtMs = m_targetBitrateUpdatedAtMs.load(std::memory_order_acquire);
            const uint64_t nowMs = steadyNowMs();
            if (updatedAtMs != 0 && nowMs >= updatedAtMs) {
                const uint64_t delayMs = nowMs - updatedAtMs;
                const uint32_t boundedDelay = delayMs > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
                    ? std::numeric_limits<uint32_t>::max()
                    : static_cast<uint32_t>(delayMs);
                m_lastBitrateApplyDelayMs.store(boundedDelay, std::memory_order_release);
            }
        }

        const bool forceKeyFrame = m_forceKeyFramePending.exchange(false, std::memory_order_acq_rel);
        av::codec::EncodedVideoFrame encoded;
                std::string encodeError;
                if (!encoder.encode(frame, encoded, forceKeyFrame, &encodeError)) {
            if (encodeError.empty()) {
                if (!loggedFirstEncodePending) {
                    loggedFirstEncodePending = true;
                    if (statusCallback) {
                        statusCallback("Video encode pending");
                    }
                }
                continue;
            }
            if (!loggedFirstEncodeError) {
                loggedFirstEncodeError = true;
                if (statusCallback) {
                    statusCallback(std::string("Video encode error: ") + encodeError);
                }
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            setErrorLocked(encodeError);
            continue;
        }
        if (!loggedFirstEncodedPacket) {
            loggedFirstEncodedPacket = true;
            if (statusCallback) {
                statusCallback("Video encoded packet observed");
            }
        }
        if (forceKeyFrame && !encoded.keyFrame) {
            m_forceKeyFramePending.store(true, std::memory_order_release);
        }

        const auto payloads = packetizeH264(encoded.payload, m_config.maxPayloadBytes);
        if (payloads.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            setErrorLocked("no RTP payloads produced from H264 frame");
            continue;
        }

        const uint32_t timestamp = static_cast<uint32_t>((encoded.pts * 90000LL) /
                                                         std::max(1, encoded.frameRate));
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            const bool marker = (i + 1 == payloads.size());
            auto packet = m_sender.buildPacket(m_config.payloadType, marker, timestamp, payloads[i]);
            if (packet.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                setErrorLocked("RTP packet build failed");
                break;
            }

            const uint16_t sequenceNumber = parseSequenceNumber(packet);

            const int sent = ::sendto(m_socket,
                                      reinterpret_cast<const char*>(packet.data()),
                                      static_cast<int>(packet.size()),
                                      0,
                                      reinterpret_cast<const sockaddr*>(&peer),
                                      sizeof(peer));
            if (sent != static_cast<int>(packet.size())) {
                std::lock_guard<std::mutex> lock(m_mutex);
                setErrorLocked("sendto failed");
                break;
            }
            m_sentPacketCount.fetch_add(1, std::memory_order_acq_rel);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                cacheSentPacketLocked(sequenceNumber, std::move(packet));
            }
            if (!loggedFirstPacket) {
                loggedFirstPacket = true;
                qInfo().noquote() << "[screen-session] first RTP sent ts=" << timestamp
                                  << "bytes=" << payloads[i].size();
                if (statusCallback) {
                    statusCallback("Video RTP packet sent");
                }
            }
        }
    }
}

void ScreenShareSession::recvLoop() {
    std::array<uint8_t, 1500> buffer{};
    bool loggedFirstPacket = false;
    bool loggedFirstEncodedPacket = false;
    bool loggedFirstEncodePending = false;
    bool loggedFirstEncodeError = false;
    const auto statusCallback = [this]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_statusCallback;
    }();
    bool loggedFirstDecodedFrame = false;
    H264AccessUnitAssembler assembler;

    while (m_running.load(std::memory_order_acquire)) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int received = ::recvfrom(m_socket,
                                        reinterpret_cast<char*>(buffer.data()),
                                        static_cast<int>(buffer.size()),
                                        0,
                                        reinterpret_cast<sockaddr*>(&from),
                                        &fromLen);
        if (received <= 0) {
            const int err = WSAGetLastError();
            if (!m_running.load(std::memory_order_acquire) || err == WSAETIMEDOUT || err == WSAEINTR) {
                continue;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            setErrorLocked("recvfrom failed");
            continue;
        }

        bool acceptSender = false;
        bool acceptRtcpFromPeerHost = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            acceptSender = shouldAcceptSenderLocked(from);
            if (!m_peerValid) {
                acceptRtcpFromPeerHost = true;
            } else {
                acceptRtcpFromPeerHost = from.sin_family == AF_INET &&
                                         from.sin_addr.s_addr == m_peer.sin_addr.s_addr;
            }
        }

        const bool isRtcp = looksLikeRtcp(buffer.data(), static_cast<std::size_t>(received));
        if (!acceptSender && (!isRtcp || !acceptRtcpFromPeerHost)) {
            continue;
        }

        if (isRtcp) {
            std::lock_guard<std::mutex> lock(m_mutex);
            (void)handleRtcpFeedbackLocked(buffer.data(), static_cast<std::size_t>(received));
            continue;
        }

        media::RTPPacket packet;
        if (!m_receiver.parsePacket(buffer.data(), static_cast<std::size_t>(received), packet)) {
            continue;
        }
        if (packet.header.payloadType != m_config.payloadType) {
            continue;
        }

        const uint32_t expectedRemoteSsrc = m_expectedRemoteVideoSsrc.load(std::memory_order_acquire);
        if (expectedRemoteSsrc != 0U && packet.header.ssrc != expectedRemoteSsrc) {
            continue;
        }

        m_receivedPacketCount.fetch_add(1, std::memory_order_acq_rel);
        if (!loggedFirstPacket) {
            loggedFirstPacket = true;
            qInfo().noquote() << "[screen-session] first RTP recv seq=" << packet.header.sequenceNumber
                              << "ts=" << packet.header.timestamp
                              << "bytes=" << packet.payload.size();
            if (statusCallback) {
                statusCallback("Video RTP packet received");
            }
        }

        av::codec::EncodedVideoFrame encoded;
        if (!assembler.consume(packet, encoded)) {
            continue;
        }
        encoded.frameRate = m_config.frameRate;

        av::codec::DecodedVideoFrame decoded;
        std::string decodeError;
        if (!m_decoder.decode(encoded, decoded, &decodeError)) {
            if (decodeError.find("Resource temporarily unavailable") == std::string::npos) {
                std::lock_guard<std::mutex> lock(m_mutex);
                setErrorLocked(decodeError.empty() ? "video decode failed" : decodeError);
            }
            continue;
        }

        std::function<void(av::codec::DecodedVideoFrame)> callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_decodedFrameCallback;
        }
        if (!loggedFirstDecodedFrame) {
            loggedFirstDecodedFrame = true;
            qInfo().noquote() << "[screen-session] first frame decoded size="
                              << decoded.width << "x" << decoded.height
                              << "pts=" << decoded.pts;
            if (statusCallback) {
                statusCallback("Video frame decoded");
            }
        }
        if (callback) {
            callback(std::move(decoded));
        }
    }
}
#endif

}  // namespace av::session















