#include "MeetingController.h"

#include "ParticipantListModel.h"
#include "UserManager.h"
#include "net/signaling/Reconnector.h"
#include "net/signaling/SignalingClient.h"
#include "storage/CallLogRepository.h"
#include "storage/MeetingRepository.h"
#include "MediaSessionManager.h"
#include "av/session/AudioCallSession.h"
#include "av/session/ScreenShareSession.h"
#include "av/capture/AudioCapture.h"
#include "av/capture/CameraCapture.h"
#include "av/render/AudioPlayer.h"
#include "av/render/VideoFrameStore.h"
#include "av/sync/AVSync.h"
#include "net/ice/StunClient.h"
#include "net/ice/TurnClient.h"
#include "security/CryptoUtils.h"
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QtMultimedia/QMediaDevices>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include "signaling.pb.h"

namespace {

constexpr quint16 kMeetMuteAll = 0x0209;
constexpr quint16 kMeetStateSync = 0x020B;
constexpr quint16 kMeetParticipantJoin = 0x020C;
constexpr quint16 kMeetParticipantLeave = 0x020D;
constexpr quint16 kMeetHostChanged = 0x020E;
constexpr quint16 kMediaRouteStatusNotify = 0x0306;
constexpr int kAudioPayloadType = 111;
constexpr int kCameraPayloadType = 96;
constexpr int kScreenPayloadType = 97;
constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 1;
constexpr int kAudioFrameSamples = 960;
constexpr int kAudioBitrate = 32000;
constexpr int kDefaultVideoWidth = 1280;
constexpr int kDefaultVideoHeight = 720;
constexpr int kDefaultVideoFrameRate = 5;
constexpr int kDefaultVideoBitrate = 1500 * 1000;
constexpr int kMaxVideoRenderDelayMsForSync = 40;
constexpr int kDefaultMaxRemoteVideoRenderQueueDepth = 8;
constexpr int kDefaultMaxPerPeerVideoRenderQueueDepth = 4;
constexpr int kDefaultMaxAudioDrivenRenderDelayMs = 40;
constexpr int kDefaultMaxVideoFramesPerDrain = 2;
constexpr int kDefaultMinVideoCadenceMs = 12;
constexpr int kDefaultMaxVideoCadenceMs = 80;
constexpr int kFixedVideoRenderTickMs = 16;
constexpr auto kSfuTransportKey = "@sfu";

struct VideoRenderTuning {
    int maxRemoteQueueDepth{kDefaultMaxRemoteVideoRenderQueueDepth};
    int maxPerPeerQueueDepth{kDefaultMaxPerPeerVideoRenderQueueDepth};
    int maxAudioDrivenRenderDelayMs{kDefaultMaxAudioDrivenRenderDelayMs};
    int maxVideoFramesPerDrain{kDefaultMaxVideoFramesPerDrain};
    int minVideoCadenceMs{kDefaultMinVideoCadenceMs};
    int maxVideoCadenceMs{kDefaultMaxVideoCadenceMs};
};

struct VideoSessionTuning {
    int width{kDefaultVideoWidth};
    int height{kDefaultVideoHeight};
    int frameRate{kDefaultVideoFrameRate};
    int bitrate{kDefaultVideoBitrate};
    QString profileName{QStringLiteral("720p")};
    av::codec::VideoEncoderPreset encoderPreset{av::codec::VideoEncoderPreset::Realtime};
};

int readBoundedEnvInt(const QProcessEnvironment& env,
                      const QString& name,
                      int fallback,
                      int minValue,
                      int maxValue) {
    const QString raw = env.value(name).trimmed();
    if (raw.isEmpty()) {
        return fallback;
    }

    bool ok = false;
    const int parsed = raw.toInt(&ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(parsed, minValue, maxValue);
}

QString canonicalVideoPresetName(const QString& raw) {
    const QString preset = raw.trimmed().toLower();
    if (preset == QStringLiteral("1080p") ||
        preset == QStringLiteral("fhd") ||
        preset == QStringLiteral("fullhd")) {
        return QStringLiteral("1080p");
    }
    if (preset == QStringLiteral("360p") || preset == QStringLiteral("sd")) {
        return QStringLiteral("360p");
    }
    return QStringLiteral("720p");
}

QString videoEncoderPresetLabel(av::codec::VideoEncoderPreset preset) {
    switch (preset) {
    case av::codec::VideoEncoderPreset::Balanced:
        return QStringLiteral("balanced");
    case av::codec::VideoEncoderPreset::Quality:
        return QStringLiteral("quality");
    case av::codec::VideoEncoderPreset::Realtime:
    default:
        return QStringLiteral("realtime");
    }
}

quint32 routeRewriteSsrc(const QString& meetingId,
                         const QString& subscriberUserId,
                         quint32 sourceSsrc,
                         bool video) {
    if (sourceSsrc == 0U) {
        return 0U;
    }

    const QByteArray key = QStringLiteral("%1|%2|%3|%4")
                               .arg(meetingId, subscriberUserId, video ? QStringLiteral("video") : QStringLiteral("audio"))
                               .arg(sourceSsrc)
                               .toUtf8();
    quint32 hash = 2166136261U;
    for (const char ch : key) {
        hash ^= static_cast<quint32>(static_cast<unsigned char>(ch));
        hash *= 16777619U;
    }
    hash &= 0x7FFFFFFFU;
    if (hash == 0U || hash == sourceSsrc) {
        hash ^= 0x5A5A5A5AU;
    }
    return hash == 0U ? 0x13572468U : hash;
}

av::codec::VideoEncoderPreset resolveVideoEncoderPreset(const QProcessEnvironment& env,
                                                        av::codec::VideoEncoderPreset fallback) {
    const QString raw = env.value(QStringLiteral("MEETING_VIDEO_ENCODER_PRESET")).trimmed().toLower();
    if (raw == QStringLiteral("balanced")) {
        return av::codec::VideoEncoderPreset::Balanced;
    }
    if (raw == QStringLiteral("quality") || raw == QStringLiteral("hq")) {
        return av::codec::VideoEncoderPreset::Quality;
    }
    if (raw == QStringLiteral("realtime") ||
        raw == QStringLiteral("speed") ||
        raw == QStringLiteral("lowlatency")) {
        return av::codec::VideoEncoderPreset::Realtime;
    }
    return fallback;
}

void applyVideoPresetDefaults(VideoSessionTuning& tuning, const QString& presetName) {
    tuning.profileName = presetName;
    if (presetName == QStringLiteral("1080p")) {
        tuning.width = 1920;
        tuning.height = 1080;
        tuning.frameRate = 8;
        tuning.bitrate = 3000 * 1000;
        tuning.encoderPreset = av::codec::VideoEncoderPreset::Balanced;
        return;
    }
    if (presetName == QStringLiteral("360p")) {
        tuning.width = 640;
        tuning.height = 360;
        tuning.frameRate = 8;
        tuning.bitrate = 700 * 1000;
        tuning.encoderPreset = av::codec::VideoEncoderPreset::Realtime;
        return;
    }

    tuning.width = kDefaultVideoWidth;
    tuning.height = kDefaultVideoHeight;
    tuning.frameRate = kDefaultVideoFrameRate;
    tuning.bitrate = kDefaultVideoBitrate;
    tuning.encoderPreset = av::codec::VideoEncoderPreset::Realtime;
}

const VideoSessionTuning& videoSessionTuning() {
    static const VideoSessionTuning tuning = [] {
        VideoSessionTuning value;
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QString presetName = canonicalVideoPresetName(env.value(QStringLiteral("MEETING_VIDEO_PRESET")));
        applyVideoPresetDefaults(value, presetName);
        value.width = readBoundedEnvInt(env,
                                        QStringLiteral("MEETING_VIDEO_WIDTH"),
                                        value.width,
                                        320,
                                        3840);
        value.height = readBoundedEnvInt(env,
                                         QStringLiteral("MEETING_VIDEO_HEIGHT"),
                                         value.height,
                                         180,
                                         2160);
        value.frameRate = readBoundedEnvInt(env,
                                            QStringLiteral("MEETING_VIDEO_FPS"),
                                            value.frameRate,
                                            1,
                                            60);
        value.bitrate = readBoundedEnvInt(env,
                                          QStringLiteral("MEETING_VIDEO_BITRATE_BPS"),
                                          value.bitrate,
                                          300 * 1000,
                                          20 * 1000 * 1000);
        value.encoderPreset = resolveVideoEncoderPreset(env, value.encoderPreset);
        return value;
    }();
    return tuning;
}

const VideoRenderTuning& videoRenderTuning() {
    static const VideoRenderTuning tuning = [] {
        VideoRenderTuning value;
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        value.maxRemoteQueueDepth = readBoundedEnvInt(env,
                                                      QStringLiteral("MEETING_VIDEO_RENDER_QUEUE_DEPTH"),
                                                      kDefaultMaxRemoteVideoRenderQueueDepth,
                                                      2,
                                                      32);
        value.maxPerPeerQueueDepth = readBoundedEnvInt(env,
                                                       QStringLiteral("MEETING_VIDEO_RENDER_QUEUE_PER_PEER_DEPTH"),
                                                       kDefaultMaxPerPeerVideoRenderQueueDepth,
                                                       1,
                                                       16);
        value.maxAudioDrivenRenderDelayMs = readBoundedEnvInt(env,
                                                               QStringLiteral("MEETING_VIDEO_AUDIO_DRIVEN_MAX_DELAY_MS"),
                                                               kDefaultMaxAudioDrivenRenderDelayMs,
                                                               10,
                                                               500);
        value.maxVideoFramesPerDrain = readBoundedEnvInt(env,
                                                         QStringLiteral("MEETING_VIDEO_MAX_FRAMES_PER_DRAIN"),
                                                         kDefaultMaxVideoFramesPerDrain,
                                                         1,
                                                         8);
        value.minVideoCadenceMs = readBoundedEnvInt(env,
                                                    QStringLiteral("MEETING_VIDEO_MIN_CADENCE_MS"),
                                                    kDefaultMinVideoCadenceMs,
                                                    1,
                                                    100);
        value.maxVideoCadenceMs = readBoundedEnvInt(env,
                                                    QStringLiteral("MEETING_VIDEO_MAX_CADENCE_MS"),
                                                    kDefaultMaxVideoCadenceMs,
                                                    value.minVideoCadenceMs,
                                                    200);
        if (value.maxVideoCadenceMs < value.minVideoCadenceMs) {
            value.maxVideoCadenceMs = value.minVideoCadenceMs;
        }
        return value;
    }();
    return tuning;
}

int suggestRemoteVideoRenderDelayMsByAudioClock(const av::codec::DecodedVideoFrame& frame,
                                                 const std::shared_ptr<av::sync::AVSync>& clock) {
    if (!clock) {
        return 0;
    }

    return av::sync::AVSync::suggestVideoRenderDelayMsByAudioClock(frame.pts,
                                                                    clock->audioPts(),
                                                                    clock->sampleRate(),
                                                                    kMaxVideoRenderDelayMsForSync);
}
QString toQtString(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

bool shouldStartVideoMutedByDefault() {
    if (!qEnvironmentVariableIsEmpty("MEETING_DEFAULT_VIDEO_MUTED")) {
        return qEnvironmentVariableIntValue("MEETING_DEFAULT_VIDEO_MUTED") != 0;
    }
    if (qEnvironmentVariableIntValue("MEETING_RUNTIME_SMOKE") != 0) {
        if (!qEnvironmentVariableIsEmpty("MEETING_SMOKE_START_VIDEO_MUTED")) {
            return qEnvironmentVariableIntValue("MEETING_SMOKE_START_VIDEO_MUTED") != 0;
        }
        return true;
    }
    return true;
}

bool shouldEmitVerboseNegotiationLogs() {
    return qEnvironmentVariableIntValue("MEETING_RUNTIME_SMOKE") != 0 ||
           qEnvironmentVariableIntValue("MEETING_VERBOSE_NEGOTIATION_LOGS") != 0;
}

struct MediaRouteStatusEvent {
    QString stage;
    QString message;
    QString route;
};

MediaRouteStatusEvent parseMediaRouteStatusEvent(const QString& rawReason) {
    MediaRouteStatusEvent event;
    const QString trimmedReason = rawReason.trimmed();
    if (trimmedReason.isEmpty()) {
        return event;
    }

    const QJsonDocument jsonDoc = QJsonDocument::fromJson(trimmedReason.toUtf8());
    if (jsonDoc.isObject()) {
        const QJsonObject obj = jsonDoc.object();
        event.stage = obj.value(QStringLiteral("stage")).toString().trimmed().toLower();
        event.message = obj.value(QStringLiteral("message")).toString().trimmed();
        event.route = obj.value(QStringLiteral("route")).toString().trimmed();
    }

    if (event.message.isEmpty()) {
        event.message = trimmedReason;
    }
    if (event.stage.isEmpty()) {
        const QString lower = trimmedReason.toLower();
        if (lower.contains(QStringLiteral("switching"))) {
            event.stage = QStringLiteral("switching");
        } else if (lower.contains(QStringLiteral("failed"))) {
            event.stage = QStringLiteral("failed");
        } else if (lower.contains(QStringLiteral("switched"))) {
            event.stage = QStringLiteral("switched");
        } else {
            event.stage = QStringLiteral("info");
        }
    }
    return event;
}

bool parseIpv4Endpoint(const QString& endpointText, QString* host, quint16* port) {
    const QString trimmed = endpointText.trimmed();
    const int colonIndex = trimmed.lastIndexOf(QLatin1Char(':'));
    if (colonIndex <= 0 || colonIndex + 1 >= trimmed.size()) {
        return false;
    }

    const QString candidateHost = trimmed.left(colonIndex).trimmed();
    const QString candidatePortText = trimmed.mid(colonIndex + 1).trimmed();
    bool ok = false;
    const int candidatePort = candidatePortText.toInt(&ok);
    if (!ok || candidatePort <= 0 || candidatePort > 65535) {
        return false;
    }

    QHostAddress parsedAddress;
    if (!parsedAddress.setAddress(candidateHost) ||
        parsedAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }

    if (host != nullptr) {
        *host = parsedAddress.toString();
    }
    if (port != nullptr) {
        *port = static_cast<quint16>(candidatePort);
    }
    return true;
}

QString resolveAdvertisedHost() {
    const QString configuredHost = qEnvironmentVariable("MEETING_ADVERTISED_HOST").trimmed();
    if (!configuredHost.isEmpty()) {
        QHostAddress configuredAddress;
        if (configuredAddress.setAddress(configuredHost) &&
            configuredAddress.protocol() == QAbstractSocket::IPv4Protocol) {
            return configuredAddress.toString();
        }
        return configuredHost;
    }

    const auto isPrivateIpv4 = [](const QHostAddress& address) {
        const quint32 raw = address.toIPv4Address();
        const quint8 a = static_cast<quint8>((raw >> 24) & 0xFFU);
        const quint8 b = static_cast<quint8>((raw >> 16) & 0xFFU);
        return a == 10U ||
               (a == 172U && b >= 16U && b <= 31U) ||
               (a == 192U && b == 168U);
    };

    const auto looksLikeVirtualOrTunnel = [](const QNetworkInterface& iface) {
        const QString tag = (iface.humanReadableName() + QLatin1Char(' ') + iface.name()).toLower();
        static const QStringList kSkipKeywords = {
            QStringLiteral("meta"),
            QStringLiteral("mihomo"),
            QStringLiteral("tap"),
            QStringLiteral("tun"),
            QStringLiteral("vpn"),
            QStringLiteral("virtual"),
            QStringLiteral("vethernet"),
            QStringLiteral("hyper-v"),
            QStringLiteral("docker")
        };
        for (const QString& keyword : kSkipKeywords) {
            if (tag.contains(keyword)) {
                return true;
            }
        }
        return false;
    };

    const auto interfaces = QNetworkInterface::allInterfaces();
    QString firstRoutableIpv4;
    for (const auto& iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            !(iface.flags() & QNetworkInterface::IsRunning) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        if (looksLikeVirtualOrTunnel(iface)) {
            continue;
        }

        for (const auto& entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback()) {
                continue;
            }
            if (isPrivateIpv4(addr)) {
                return addr.toString();
            }
            if (firstRoutableIpv4.isEmpty()) {
                firstRoutableIpv4 = addr.toString();
            }
        }
    }

    if (!firstRoutableIpv4.isEmpty()) {
        return firstRoutableIpv4;
    }

    for (const auto& iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            !(iface.flags() & QNetworkInterface::IsRunning) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const auto& entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback()) {
                continue;
            }
            return addr.toString();
        }
    }

    return QStringLiteral("127.0.0.1");
}

QString makeIceToken(int randomBytes) {
    return security::CryptoUtils::generateRandomToken(randomBytes);
}

QString colonDelimitedSha256Fingerprint(const QString& seed) {
    const QString hex = security::CryptoUtils::sha256Hex(seed.toUtf8()).toUpper();
    QString fingerprint;
    fingerprint.reserve(hex.size() + (hex.size() / 2));
    for (int i = 0; i < hex.size(); i += 2) {
        if (!fingerprint.isEmpty()) {
            fingerprint.append(QLatin1Char(':'));
        }
        fingerprint.append(hex.mid(i, 2));
    }
    return fingerprint;
}

QString makeClientDtlsFingerprint(const QString& meetingId, const QString& userId, const QString& mediaKind) {
    const QString seed = QStringLiteral("%1:%2:%3:%4")
                             .arg(meetingId,
                                  userId,
                                  mediaKind,
                                  security::CryptoUtils::generateRandomToken(32));
    return colonDelimitedSha256Fingerprint(seed);
}

QString makeHostIceCandidate(const QString& host, quint16 port) {
    if (host.trimmed().isEmpty() || port == 0) {
        return {};
    }
    return QStringLiteral("candidate:1 1 udp 2130706431 %1 %2 typ host").arg(host.trimmed()).arg(port);
}

enum class IcePolicy {
    All,
    RelayOnly,
};

IcePolicy icePolicyFromEnvironment() {
    const QString value = QProcessEnvironment::systemEnvironment()
                              .value(QStringLiteral("MEETING_ICE_POLICY"), QStringLiteral("all"))
                              .trimmed()
                              .toLower();
    return value == QStringLiteral("relay-only") ? IcePolicy::RelayOnly : IcePolicy::All;
}

QStringList makeHostIceCandidates(quint16 localPort) {
    const QString candidate = makeHostIceCandidate(resolveAdvertisedHost(), localPort);
    return candidate.isEmpty() ? QStringList{} : QStringList{candidate};
}

QStringList splitIceServerEntry(const QString& entry) {
    return entry.split(QLatin1Char('|'), Qt::KeepEmptyParts);
}

struct TurnRelaySettings {
    bool valid{false};
    QString host{};
    quint16 port{0};
    QString username{};
    QString credential{};
};

TurnRelaySettings firstTurnRelaySettings(const QStringList& iceServers) {
    for (const QString& entry : iceServers) {
        const QStringList parts = splitIceServerEntry(entry);
        const QString url = parts.value(0).trimmed();
        if (!url.startsWith(QStringLiteral("turn:"), Qt::CaseInsensitive) &&
            !url.startsWith(QStringLiteral("turns:"), Qt::CaseInsensitive)) {
            continue;
        }

        ice::TurnServerConfig turn;
        if (!ice::TurnClient::parseTurnServerUrl(url, &turn)) {
            continue;
        }
        const QString username = parts.value(1).trimmed();
        const QString credential = parts.value(2).trimmed();
        if (username.isEmpty() || credential.isEmpty()) {
            continue;
        }
        return TurnRelaySettings{true, turn.host, turn.port, username, credential};
    }
    return {};
}

QStringList collectIceCandidates(quint16 localPort, const QStringList& iceServers, IcePolicy policy) {
    QStringList candidates;
    if (policy != IcePolicy::RelayOnly) {
        candidates.append(makeHostIceCandidates(localPort));
    }

    for (const QString& entry : iceServers) {
        const QStringList parts = splitIceServerEntry(entry);
        const QString url = parts.value(0).trimmed();
        if (url.startsWith(QStringLiteral("stun:"), Qt::CaseInsensitive) && policy != IcePolicy::RelayOnly) {
            QString host;
            quint16 port = 0;
            if (!ice::parseStunServerUrl(url, &host, &port)) {
                continue;
            }
            ice::StunClient stun;
            stun.setTimeoutMs(150);
            stun.setRetryCount(1);
            const ice::StunBindingResult result = stun.bindingRequest(host, port, localPort);
            if (result.success) {
                const QString candidate = ice::makeServerReflexiveCandidate(result.mappedAddress.address,
                                                                            result.mappedAddress.port);
                if (!candidate.isEmpty()) {
                    candidates.append(candidate);
                }
            }
            continue;
        }

        if (url.startsWith(QStringLiteral("turn:"), Qt::CaseInsensitive) ||
            url.startsWith(QStringLiteral("turns:"), Qt::CaseInsensitive)) {
            ice::TurnServerConfig turn;
            if (!ice::TurnClient::parseTurnServerUrl(url, &turn)) {
                continue;
            }
            QHostAddress relayAddress(turn.host);
            if (relayAddress.isNull()) {
                continue;
            }
            const QString candidate = ice::TurnClient::makeRelayCandidate(relayAddress, turn.port);
            if (!candidate.isEmpty()) {
                candidates.append(candidate);
            }
        }
    }

    candidates.removeDuplicates();
    return candidates;
}

std::vector<uint8_t> buildStunBindingRequest(const QString& username) {
    const QByteArray usernameBytes = username.trimmed().toUtf8();
    if (usernameBytes.isEmpty() || usernameBytes.size() > 255) {
        return {};
    }

    const auto attributeLength = static_cast<std::size_t>(usernameBytes.size());
    const std::size_t paddedAttributeLength = (attributeLength + 3U) & ~std::size_t{3U};
    const std::size_t messageLength = 4U + paddedAttributeLength;
    std::vector<uint8_t> packet(20U + messageLength, 0U);

    packet[0] = 0x00U;
    packet[1] = 0x01U;
    packet[2] = static_cast<uint8_t>((messageLength >> 8U) & 0xFFU);
    packet[3] = static_cast<uint8_t>(messageLength & 0xFFU);
    packet[4] = 0x21U;
    packet[5] = 0x12U;
    packet[6] = 0xA4U;
    packet[7] = 0x42U;
    for (std::size_t i = 8U; i < 20U; ++i) {
        packet[i] = static_cast<uint8_t>(QRandomGenerator::global()->generate() & 0xFFU);
    }

    packet[20] = 0x00U;
    packet[21] = 0x06U;
    packet[22] = static_cast<uint8_t>((attributeLength >> 8U) & 0xFFU);
    packet[23] = static_cast<uint8_t>(attributeLength & 0xFFU);
    std::memcpy(packet.data() + 24U,
                usernameBytes.constData(),
                static_cast<std::size_t>(usernameBytes.size()));
    return packet;
}

QString normalizePreferredCameraDevice(const QString& deviceName) {
    return deviceName.trimmed();
}

QString cameraDeviceName(const QCameraDevice& device) {
    const QString description = device.description().trimmed();
    if (!description.isEmpty()) {
        return description;
    }
    return QString::fromUtf8(device.id()).trimmed();
}

QStringList readAvailableCameraDeviceNames() {
    return av::capture::CameraCapture::availableDeviceNames();
}
QString normalizePreferredAudioDevice(const QString& deviceName) {
    return deviceName.trimmed();
}

QStringList readAvailableAudioInputDeviceNames() {
    return av::capture::AudioCapture::availableInputDevices();
}

QStringList readAvailableAudioOutputDeviceNames() {
    return av::render::AudioPlayer::availableOutputDevices();
}

}  // namespace
MeetingController::MeetingController(const QString& databasePath, QObject* parent)
    : QObject(parent)
    , m_userManager(new UserManager(databasePath, this))
    , m_participantModel(new ParticipantListModel(this))
    , m_meetingRepository(std::make_unique<MeetingRepository>(databasePath))
    , m_callLogRepository(std::make_unique<CallLogRepository>(databasePath))
    , m_signaling(new signaling::SignalingClient(this))
    , m_reconnector(new signaling::Reconnector(this))
    , m_heartbeatTimer(new QTimer(this))
    , m_videoRenderTimer(new QTimer(this))
    , m_mediaDevices(new QMediaDevices(this)) {
    m_reconnector->configure(1000, 30000);

    m_serverHost = m_userManager->serverHost();
    m_serverPort = m_userManager->serverPort();
    m_username = m_userManager->username();
    m_userId = m_userManager->userId();
        const QString storedPreferredCameraDevice = normalizePreferredCameraDevice(m_userManager->preferredCameraDevice());
    m_preferredCameraDevice = storedPreferredCameraDevice.isEmpty()
                                  ? qEnvironmentVariable("MEETING_CAMERA_DEVICE_NAME").trimmed()
                                  : storedPreferredCameraDevice;
    const QString storedPreferredMicrophone = normalizePreferredAudioDevice(m_userManager->preferredMicrophoneDevice());
    m_preferredMicrophoneDevice = storedPreferredMicrophone.isEmpty()
                                      ? qEnvironmentVariable("MEETING_AUDIO_INPUT_DEVICE_NAME").trimmed()
                                      : storedPreferredMicrophone;
    const QString storedPreferredSpeaker = normalizePreferredAudioDevice(m_userManager->preferredSpeakerDevice());
    m_preferredSpeakerDevice = storedPreferredSpeaker.isEmpty()
                                   ? qEnvironmentVariable("MEETING_AUDIO_OUTPUT_DEVICE_NAME").trimmed()
                                   : storedPreferredSpeaker;
    refreshAvailableCameraDevices();
    refreshAvailableAudioDevices();
    connect(m_mediaDevices, &QMediaDevices::videoInputsChanged, this, &MeetingController::refreshAvailableCameraDevices);
    connect(m_mediaDevices, &QMediaDevices::audioInputsChanged, this, &MeetingController::refreshAvailableAudioDevices);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged, this, &MeetingController::refreshAvailableAudioDevices);

    m_heartbeatTimer->setInterval(30 * 1000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (m_loggedIn) {
            m_signaling->sendHeartbeat(QDateTime::currentMSecsSinceEpoch());
        }
    });

    m_videoRenderTimer->setSingleShot(false);
    m_videoRenderTimer->setInterval(kFixedVideoRenderTickMs);
    connect(m_videoRenderTimer, &QTimer::timeout, this, [this]() {
        drainRemoteVideoRenderQueue();
    });

    m_audioSessionManager = std::make_unique<MediaSessionManager>();
    m_mediaSessionManager = std::make_unique<MediaSessionManager>();
    m_localVideoFrameStore = std::make_unique<av::render::VideoFrameStore>();
    m_remoteScreenFrameStore = std::make_unique<av::render::VideoFrameStore>();
    m_remoteVideoFrameStore = std::make_unique<av::render::VideoFrameStore>();
    connect(m_signaling, &signaling::SignalingClient::connectedChanged, this, [this](bool connected) {
        if (connected) {
            if (m_reconnecting) {
                m_reconnecting = false;
                emit reconnectingChanged();
            }
            m_reconnector->reset();
            setStatusText(QStringLiteral("Connected"));
            emit connectedChanged();
            sendPendingLoginIfReady("connected");
            return;
        }

        if (m_waitingLogin || m_loggedIn || m_shouldStayConnected) {
            m_loginRequestSent = false;
        }
        const bool hadMeetingState = m_inMeeting || !m_meetingId.isEmpty() || !m_meetingTitle.isEmpty() || m_participantModel->rowCount() > 0;
        m_heartbeatTimer->stop();
        m_waitingLeaveResponse = false;
        if (hadMeetingState) {
            resetMeetingState(QStringLiteral("网络断开"));
            emit infoMessage(QStringLiteral("Connection lost, left the meeting"));
        }

        if (m_shouldStayConnected) {
            if (!m_reconnecting) {
                m_reconnecting = true;
                emit reconnectingChanged();
            }
            setStatusText(QStringLiteral("Reconnecting..."));
            m_reconnector->schedule();
        } else {
            setStatusText(QStringLiteral("Disconnected"));
            emit connectedChanged();
        }
    });

    connect(m_reconnector, &signaling::Reconnector::reconnectRequested, this, [this]() {
        m_signaling->reconnect();
    });

    connect(this, &MeetingController::infoMessage, this, [this](const QString& message) {
        qInfo().noquote() << "[meeting]" << message;
        setStatusText(message);
    });

    connect(m_signaling, &signaling::SignalingClient::protocolError, this, [this](const QString& message) {
        setStatusText(message);
        emit infoMessage(message);
    });

    connect(m_signaling, &signaling::SignalingClient::loginFinished, this,
            [this](bool success, const QString& userId, const QString& token, const QString& error) {
        const bool wasRestoringSession = m_restoringSession;
        m_waitingLogin = false;
        m_restoringSession = false;

        if (!success) {
            m_loginRequestSent = false;
            if (wasRestoringSession) {
                m_passwordHash.clear();
                m_userManager->clearToken();
            }

            const QString msg = wasRestoringSession
                                    ? QStringLiteral("Session expired, please login again")
                                    : (error.isEmpty() ? QStringLiteral("Login failed") : error);
            m_shouldStayConnected = false;
            m_reconnector->stop();
            if (m_reconnecting) {
                m_reconnecting = false;
                emit reconnectingChanged();
            }
            if (m_loggedIn) {
                m_loggedIn = false;
                emit loggedInChanged();
            }
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_userId = userId;
        m_passwordHash = token;
        m_userManager->setServerEndpoint(m_serverHost, m_serverPort);
        m_userManager->setSession(m_username, userId, token);
        emit sessionChanged();

        if (!m_loggedIn) {
            m_loggedIn = true;
            emit loggedInChanged();
        }
        m_shouldStayConnected = true;
        m_reconnector->reset();
        m_heartbeatTimer->start();
        setStatusText(QStringLiteral("Login success"));
        emit infoMessage(QStringLiteral("Login success"));
    });

    connect(m_signaling, &signaling::SignalingClient::heartbeatReceived, this, [this](qint64 serverTimestampMs) {
        Q_UNUSED(serverTimestampMs)
        if (m_loggedIn && m_statusText != QStringLiteral("Reconnecting...")) {
            setStatusText(QStringLiteral("Online"));
        }
    });

    connect(m_signaling, &signaling::SignalingClient::createMeetingFinished, this,
            [this](bool success, const QString& meetingId, const QString& error) {
        if (!success) {
            m_pendingMeetingTitle.clear();
            const QString msg = error.isEmpty() ? QStringLiteral("Create meeting failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_meetingId = meetingId;
        m_meetingTitle = m_pendingMeetingTitle;
        m_pendingMeetingTitle.clear();
        m_inMeeting = true;
        m_audioMuted = false;
        m_videoMuted = shouldStartVideoMutedByDefault();
        setScreenSharing(false);
        m_waitingLeaveResponse = false;
        m_currentMeetingHost = true;
        m_participantModel->clearParticipants();
        ensureLocalParticipant(true);
        persistMeetingSessionStart(true);
        syncParticipantsChanged();

        emit meetingIdChanged();
        emit meetingTitleChanged();
        emit inMeetingChanged();
        emit audioMutedChanged();
        emit videoMutedChanged();

        const QString msg = QStringLiteral("Meeting created: %1").arg(m_meetingId);
        setStatusText(msg);
        emit infoMessage(msg);
    });

    connect(m_signaling, &signaling::SignalingClient::joinMeetingFinished, this,
            [this](bool success, const QString& meetingId, const QString& title, const QString& sfuAddress, const QStringList& iceServers, const QStringList& participants, const QString& hostUserId, const QString& error) {
        if (!success) {
            const QString msg = error.isEmpty() ? QStringLiteral("Join meeting failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_meetingId = meetingId;
        m_meetingTitle = title;
        m_iceServers = iceServers;
        updateSfuRoute(sfuAddress);
        m_inMeeting = true;
        m_audioMuted = false;
        m_videoMuted = shouldStartVideoMutedByDefault();
        setScreenSharing(false);
        m_waitingLeaveResponse = false;
        const bool isHost = !hostUserId.isEmpty() && hostUserId == m_userId;
        m_currentMeetingHost = isHost;
        m_participantModel->replaceParticipantsFromDisplayList(participants);
        if (!hostUserId.isEmpty()) {
            m_participantModel->setHostUserId(hostUserId);
        }
        ensureLocalParticipant(isHost);
        persistMeetingSessionStart(isHost);
        syncParticipantsChanged();

        emit meetingIdChanged();
        emit meetingTitleChanged();
        emit inMeetingChanged();
        emit audioMutedChanged();
        emit videoMutedChanged();

        const QString msg = QStringLiteral("Joined meeting: %1").arg(m_meetingId);
        setStatusText(msg);
        emit infoMessage(msg);
    });

    connect(m_signaling, &signaling::SignalingClient::leaveMeetingFinished, this,
            [this](bool success, const QString& error) {
        const bool wasWaitingLeave = m_waitingLeaveResponse;
        m_waitingLeaveResponse = false;

        if (!success) {
            const QString msg = error.isEmpty() ? QStringLiteral("Leave meeting failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        if (!wasWaitingLeave && !m_inMeeting) {
            return;
        }

        resetMeetingState(QStringLiteral("主动离开"));
        setStatusText(QStringLiteral("Left meeting"));
        emit infoMessage(QStringLiteral("Left meeting"));
    });

    connect(m_signaling, &signaling::SignalingClient::kicked, this, [this](const QString& reason) {
        handleSessionKicked(reason);
    });

    connect(m_signaling, &signaling::SignalingClient::chatSendFinished, this,
            [this](bool success, const QString& messageId, const QString& error) {
        if (!success) {
            const QString msg = error.isEmpty() ? QStringLiteral("Send chat failed") : error;
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        emit infoMessage(QStringLiteral("Chat sent: %1").arg(messageId));
    });

    connect(m_signaling, &signaling::SignalingClient::chatReceived, this,
            [this](const QString& senderId,
                   const QString& senderName,
                   int type,
                   const QString& content,
                   const QString& replyToId,
                   qint64 timestamp) {
        Q_UNUSED(senderId)
        Q_UNUSED(type)
        Q_UNUSED(replyToId)
        Q_UNUSED(timestamp)
        emit infoMessage(QStringLiteral("[%1] %2").arg(senderName, content));
    });

    connect(m_signaling, &signaling::SignalingClient::mediaTransportAnswerReceived, this,
            [this](const QString& meetingId,
                   const QString& serverIceUfrag,
                   const QString& serverIcePwd,
                   const QString& serverDtlsFingerprint,
                   const QStringList& serverCandidates,
                   quint32 assignedAudioSsrc,
                   quint32 assignedVideoSsrc) {
        handleMediaTransportAnswer(meetingId,
                                   serverIceUfrag,
                                   serverIcePwd,
                                   serverDtlsFingerprint,
                                   serverCandidates,
                                   assignedAudioSsrc,
                                   assignedVideoSsrc);
    });
    connect(m_signaling, &signaling::SignalingClient::protobufMessageReceived, this,
            [this](quint16 signalType, const QByteArray& payload) {
        handleProtobufMessage(signalType, payload);
    });

    restoreCachedSession();
}

MeetingController::~MeetingController() = default;

bool MeetingController::loggedIn() const {
    return m_loggedIn;
}

bool MeetingController::reconnecting() const {
    return m_reconnecting;
}

bool MeetingController::connected() const {
    return m_signaling->isConnected();
}

bool MeetingController::inMeeting() const {
    return m_inMeeting;
}

bool MeetingController::audioMuted() const {
    return m_audioMuted;
}

bool MeetingController::videoMuted() const {
    return m_videoMuted;
}

bool MeetingController::screenSharing() const {
    return m_screenSharing;
}

QString MeetingController::activeAudioPeerUserId() const {
    return currentAudioPeerUserId();
}

QString MeetingController::activeShareUserId() const {
    return m_activeShareUserId;
}

QString MeetingController::activeVideoPeerUserId() const {
    return m_activeVideoPeerUserId;
}

QString MeetingController::activeShareDisplayName() const {
    return m_activeShareDisplayName;
}

bool MeetingController::hasActiveShare() const {
    return !m_activeShareUserId.isEmpty();
}

QObject* MeetingController::localVideoFrameSource() const {
    return m_localVideoFrameStore.get();
}

QObject* MeetingController::remoteScreenFrameSource() const {
    return m_remoteScreenFrameStore.get();
}

QObject* MeetingController::remoteVideoFrameSource() const {
    const QString normalized = m_activeVideoPeerUserId.trimmed();
    if (!normalized.isEmpty()) {
        const auto it = m_remoteVideoFrameStoresByPeer.constFind(normalized);
        if (it != m_remoteVideoFrameStoresByPeer.constEnd() && it.value() != nullptr) {
            return it.value();
        }
    }
    return m_remoteVideoFrameStore.get();
}

QObject* MeetingController::remoteVideoFrameSourceForUser(const QString& userId) const {
    const QString normalized = userId.trimmed();
    if (normalized.isEmpty() || normalized == m_userId || !hasRemoteVideoParticipant(normalized)) {
        return nullptr;
    }

    const auto it = m_remoteVideoFrameStoresByPeer.constFind(normalized);
    if (it == m_remoteVideoFrameStoresByPeer.constEnd()) {
        return nullptr;
    }
    return it.value();
}

quint32 MeetingController::remoteVideoSsrcForUser(const QString& userId) const {
    const QString normalized = userId.trimmed();
    if (normalized.isEmpty() || normalized == m_userId || !hasRemoteVideoParticipant(normalized)) {
        return 0U;
    }

    const auto it = m_remoteVideoSsrcByPeer.constFind(normalized);
    if (it == m_remoteVideoSsrcByPeer.constEnd()) {
        return 0U;
    }

    return *it;
}

QString MeetingController::username() const {
    return m_username;
}

QString MeetingController::userId() const {
    return m_userId;
}

QString MeetingController::meetingId() const {
    return m_meetingId;
}

QString MeetingController::meetingTitle() const {
    return m_meetingTitle;
}

QAbstractItemModel* MeetingController::participantModel() const {
    return m_participantModel;
}

QStringList MeetingController::participants() const {
    return m_participantModel->displayNames();
}

QStringList MeetingController::availableCameraDevices() const {
    return m_availableCameraDevices;
}

QString MeetingController::preferredCameraDevice() const {
    return m_preferredCameraDevice;
}

QStringList MeetingController::availableAudioInputDevices() const {
    return m_availableAudioInputDevices;
}

QStringList MeetingController::availableAudioOutputDevices() const {
    return m_availableAudioOutputDevices;
}

QString MeetingController::preferredMicrophoneDevice() const {
    return m_preferredMicrophoneDevice;
}

QString MeetingController::preferredSpeakerDevice() const {
    return m_preferredSpeakerDevice;
}

QString MeetingController::statusText() const {
    return m_statusText;
}

quint64 MeetingController::audioSentPacketCount() const {
    if (!m_audioCallSession) {
        return 0;
    }
    return static_cast<quint64>(m_audioCallSession->sentPacketCount());
}

quint64 MeetingController::audioReceivedPacketCount() const {
    if (!m_audioCallSession) {
        return 0;
    }
    return static_cast<quint64>(m_audioCallSession->receivedPacketCount());
}

quint64 MeetingController::audioPlayedFrameCount() const {
    if (!m_audioCallSession) {
        return 0;
    }
    return static_cast<quint64>(m_audioCallSession->playedFrameCount());
}

quint32 MeetingController::audioLastRttMs() const {
    if (!m_audioCallSession) {
        return 0;
    }
    return static_cast<quint32>(m_audioCallSession->lastRttMs());
}

quint32 MeetingController::audioTargetBitrateBps() const {
    if (!m_audioCallSession) {
        return 0;
    }
    return static_cast<quint32>(m_audioCallSession->targetBitrateBps());
}

bool MeetingController::audioIceConnected() const {
    return m_audioCallSession && m_audioCallSession->iceConnected();
}

bool MeetingController::audioDtlsConnected() const {
    return m_audioCallSession && m_audioCallSession->dtlsConnected();
}

bool MeetingController::audioSrtpReady() const {
    return m_audioCallSession && m_audioCallSession->srtpReady();
}

bool MeetingController::videoIceConnected() const {
    return m_screenShareSession && m_screenShareSession->iceConnected();
}

bool MeetingController::videoDtlsConnected() const {
    return m_screenShareSession && m_screenShareSession->dtlsConnected();
}

bool MeetingController::videoSrtpReady() const {
    return m_screenShareSession && m_screenShareSession->srtpReady();
}

qint64 MeetingController::videoLastAudioSkewMs() const {
    if (!m_hasVideoAudioSkewSample) {
        return 0;
    }
    return m_lastVideoAudioSkewMs;
}

qint64 MeetingController::videoMaxAbsAudioSkewMs() const {
    if (!m_hasVideoAudioSkewSample) {
        return 0;
    }
    return m_maxAbsVideoAudioSkewMs;
}

quint64 MeetingController::videoAudioSkewSampleCount() const {
    return m_videoAudioSkewSampleCount;
}

quint64 MeetingController::videoAudioSkewCandidateCount() const {
    return m_videoAudioSkewCandidateCount;
}

quint64 MeetingController::videoAudioSkewNoClockCount() const {
    return m_videoAudioSkewNoClockCount;
}

quint64 MeetingController::videoAudioSkewInvalidVideoPtsCount() const {
    return m_videoAudioSkewInvalidVideoPtsCount;
}

quint64 MeetingController::videoAudioSkewInvalidAudioClockCount() const {
    return m_videoAudioSkewInvalidAudioClockCount;
}

quint64 MeetingController::remoteVideoDecodedFrameCount() const {
    return m_remoteVideoDecodedFrameCount;
}

quint64 MeetingController::remoteVideoDroppedByAudioClockCount() const {
    return m_remoteVideoDroppedByAudioClockCount;
}

quint64 MeetingController::remoteVideoQueuedFrameCount() const {
    return m_remoteVideoQueuedFrameCount;
}

quint64 MeetingController::remoteVideoRenderedFrameCount() const {
    return m_remoteVideoRenderedFrameCount;
}

quint64 MeetingController::remoteVideoStalePtsDropCount() const {
    return m_remoteVideoStalePtsDropCount;
}

quint64 MeetingController::remoteVideoRescheduledFrameCount() const {
    return m_remoteVideoRescheduledFrameCount;
}

quint64 MeetingController::remoteVideoQueueResetCount() const {
    return m_remoteVideoQueueResetCount;
}

void MeetingController::shutdownMediaForRuntimeSmoke() {
    resetMediaNegotiation();
}

void MeetingController::login(const QString& username, const QString& password) {
    if (username.trimmed().isEmpty() || password.isEmpty()) {
        setStatusText(QStringLiteral("Username/password required"));
        return;
    }

    m_username = username.trimmed();
    m_passwordHash = password;
    m_shouldStayConnected = true;
    m_waitingLogin = true;
    m_loginRequestSent = false;
    m_restoringSession = false;
    m_userManager->setServerEndpoint(m_serverHost, m_serverPort);
    emit sessionChanged();

    setStatusText(QStringLiteral("Connecting..."));

    if (m_signaling->isConnected()) {
        sendPendingLoginIfReady("already-connected");
    } else {
        m_signaling->connectToServer(m_serverHost, m_serverPort);
        QTimer::singleShot(250, this, [this]() {
            sendPendingLoginIfReady("connect-fallback-250ms");
        });
        QTimer::singleShot(1000, this, [this]() {
            sendPendingLoginIfReady("connect-fallback-1000ms");
        });
    }
}

void MeetingController::sendPendingLoginIfReady(const char* reason) {
    if (!m_signaling || !m_signaling->isConnected()) {
        if (qEnvironmentVariableIntValue("MEETING_RUNTIME_SMOKE") != 0) {
            qInfo().noquote() << "[meeting]" << "pending login wait: socket not connected"
                              << "reason=" << reason
                              << "server=" << QStringLiteral("%1:%2").arg(m_serverHost).arg(m_serverPort);
        }
        return;
    }

    const bool hasCredentials = !m_username.isEmpty() && !m_passwordHash.isEmpty();
    const bool shouldResumeOnReconnect = m_shouldStayConnected && m_loggedIn;
    if (!hasCredentials || (!m_waitingLogin && !shouldResumeOnReconnect)) {
        if (qEnvironmentVariableIntValue("MEETING_RUNTIME_SMOKE") != 0) {
            qInfo().noquote() << "[meeting]" << "pending login skipped"
                              << "reason=" << reason
                              << "has_credentials=" << hasCredentials
                              << "waiting_login=" << m_waitingLogin
                              << "resume=" << shouldResumeOnReconnect;
        }
        return;
    }

    if (m_loginRequestSent) {
        if (qEnvironmentVariableIntValue("MEETING_RUNTIME_SMOKE") != 0) {
            qInfo().noquote() << "[meeting]" << "pending login skipped: request already sent"
                              << "reason=" << reason;
        }
        return;
    }

    const bool useResumeToken = m_restoringSession || !m_waitingLogin;
    m_loginRequestSent = true;
    if (qEnvironmentVariableIntValue("MEETING_RUNTIME_SMOKE") != 0) {
        qInfo().noquote() << "[meeting]" << "sending pending login"
                          << "reason=" << reason
                          << "username=" << m_username
                          << "resume=" << useResumeToken
                          << "server=" << QStringLiteral("%1:%2").arg(m_serverHost).arg(m_serverPort);
    }
    m_signaling->login(m_username,
                       m_passwordHash,
                       QStringLiteral("qt-client"),
                       QStringLiteral("desktop"),
                       useResumeToken);
}

void MeetingController::setServerEndpoint(const QString& host, quint16 port) {
    const QString normalizedHost = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    const quint16 normalizedPort = port == 0 ? 8443 : port;
    if (m_serverHost == normalizedHost && m_serverPort == normalizedPort) {
        return;
    }

    m_serverHost = normalizedHost;
    m_serverPort = normalizedPort;
    if (m_userManager) {
        m_userManager->setServerEndpoint(m_serverHost, m_serverPort);
    }
}

bool MeetingController::setPreferredCameraDevice(const QString& deviceName) {
    const QString normalized = normalizePreferredCameraDevice(deviceName);
    if (m_preferredCameraDevice == normalized) {
        return true;
    }

    m_preferredCameraDevice = normalized;
    if (m_userManager) {
        m_userManager->setPreferredCameraDevice(normalized);
    }
    emit preferredCameraDeviceChanged();

    if (!m_screenShareSession) {
        return true;
    }

    if (!m_screenShareSession->setPreferredCameraDeviceName(normalized.toStdString())) {
        emit infoMessage(QStringLiteral("Camera device switch pending restart"));
        return false;
    }
    return true;
}
bool MeetingController::setPreferredMicrophoneDevice(const QString& deviceName) {
    const QString normalized = normalizePreferredAudioDevice(deviceName);
    if (m_preferredMicrophoneDevice == normalized) {
        return true;
    }

    m_preferredMicrophoneDevice = normalized;
    if (m_userManager) {
        m_userManager->setPreferredMicrophoneDevice(normalized);
    }
    emit preferredMicrophoneDeviceChanged();

    if (m_audioCallSession && !m_audioCallSession->setPreferredInputDeviceName(normalized)) {
        emit infoMessage(QStringLiteral("Microphone switch failed, keeping previous device"));
        return false;
    }

    return true;
}

bool MeetingController::setPreferredSpeakerDevice(const QString& deviceName) {
    const QString normalized = normalizePreferredAudioDevice(deviceName);
    if (m_preferredSpeakerDevice == normalized) {
        return true;
    }

    m_preferredSpeakerDevice = normalized;
    if (m_userManager) {
        m_userManager->setPreferredSpeakerDevice(normalized);
    }
    emit preferredSpeakerDeviceChanged();

    if (m_audioCallSession && !m_audioCallSession->setPreferredOutputDeviceName(normalized)) {
        emit infoMessage(QStringLiteral("Speaker switch failed, keeping previous device"));
        return false;
    }

    return true;
}


void MeetingController::refreshAvailableCameraDevices() {
    const QStringList nextDevices = readAvailableCameraDeviceNames();
    if (m_availableCameraDevices == nextDevices) {
        return;
    }

    m_availableCameraDevices = nextDevices;
    emit availableCameraDevicesChanged();
}
void MeetingController::refreshAvailableAudioDevices() {
    const QStringList nextInputs = readAvailableAudioInputDeviceNames();
    if (m_availableAudioInputDevices != nextInputs) {
        m_availableAudioInputDevices = nextInputs;
        emit availableAudioInputDevicesChanged();
    }

    const QStringList nextOutputs = readAvailableAudioOutputDeviceNames();
    if (m_availableAudioOutputDevices != nextOutputs) {
        m_availableAudioOutputDevices = nextOutputs;
        emit availableAudioOutputDevicesChanged();
    }
}

void MeetingController::logout() {
    m_shouldStayConnected = false;
    m_waitingLogin = false;
    m_loginRequestSent = false;
    m_restoringSession = false;
    m_waitingLeaveResponse = false;
    m_pendingMeetingTitle.clear();
    m_reconnector->stop();
    m_heartbeatTimer->stop();
    resetMeetingState(QStringLiteral("主动登出"));
    m_passwordHash.clear();
    m_username.clear();
    m_userId.clear();
    m_userManager->clearSession();
    emit sessionChanged();

    if (m_reconnecting) {
        m_reconnecting = false;
        emit reconnectingChanged();
    }

    if (m_loggedIn) {
        m_loggedIn = false;
        emit loggedInChanged();
    }

    m_signaling->disconnectFromServer();
    setStatusText(QStringLiteral("Logged out"));
}

void MeetingController::createMeeting(const QString& title, const QString& password, int maxParticipants) {
    if (!m_loggedIn) {
        setStatusText(QStringLiteral("Please login first"));
        return;
    }

    m_pendingMeetingTitle = title.trimmed();
    if (m_pendingMeetingTitle.isEmpty()) {
        setStatusText(QStringLiteral("Meeting title required"));
        return;
    }

    const int boundedParticipants = qMax(1, maxParticipants);
    m_signaling->createMeeting(m_pendingMeetingTitle, password, boundedParticipants);
    setStatusText(QStringLiteral("Creating meeting..."));
}

void MeetingController::joinMeeting(const QString& meetingId, const QString& password) {
    if (!m_loggedIn) {
        setStatusText(QStringLiteral("Please login first"));
        return;
    }

    if (meetingId.trimmed().isEmpty()) {
        setStatusText(QStringLiteral("Meeting ID required"));
        return;
    }

    m_pendingMeetingTitle.clear();
    m_signaling->joinMeeting(meetingId.trimmed(), password);
    setStatusText(QStringLiteral("Joining meeting..."));
}

void MeetingController::leaveMeeting() {
    if (!m_inMeeting && m_meetingId.isEmpty() && m_participantModel->rowCount() == 0) {
        return;
    }

    if (m_waitingLeaveResponse) {
        return;
    }

    if (!m_signaling->isConnected()) {
        resetMeetingState(QStringLiteral("主动离开"));
        setStatusText(QStringLiteral("Left meeting"));
        emit infoMessage(QStringLiteral("Left meeting"));
        return;
    }

    m_waitingLeaveResponse = true;
    m_signaling->leaveMeeting();
    setStatusText(QStringLiteral("Leaving meeting..."));
}

void MeetingController::toggleAudio() {
    if (!m_inMeeting) {
        return;
    }
    m_audioMuted = !m_audioMuted;
    if (m_signaling->isConnected()) {
        m_signaling->sendMediaMuteToggle(0, m_audioMuted);
    }
    if (m_audioCallSession) {
        m_audioCallSession->setCaptureMuted(m_audioMuted);
    (void)m_audioCallSession->setPreferredInputDeviceName(m_preferredMicrophoneDevice);
    (void)m_audioCallSession->setPreferredOutputDeviceName(m_preferredSpeakerDevice);
    }
    if (!m_audioMuted) {
        maybeStartMediaNegotiation();
    }
    if (m_signaling->isConnected()) {
        sendAudioOfferToPeer(true);
    }
    syncLocalParticipantMediaState();
    emit audioMutedChanged();
}

void MeetingController::toggleVideo() {
    if (!m_inMeeting) {
        return;
    }
    m_videoMuted = !m_videoMuted;
    if (m_signaling->isConnected()) {
        m_signaling->sendMediaMuteToggle(1, m_videoMuted);
    }
    if (!m_videoMuted) {
        maybeStartMediaNegotiation();
    } else if (m_localVideoFrameStore) {
        m_localVideoFrameStore->clear();
    }
    updateVideoSessionSettings();
    if (m_signaling->isConnected()) {
        sendVideoOfferToPeer(true);
    }
    syncLocalParticipantMediaState();
    emit videoMutedChanged();
}

void MeetingController::toggleScreenSharing() {
    if (!m_inMeeting) {
        return;
    }

    const bool nextSharing = !m_screenSharing;
    if (nextSharing) {
        setScreenSharing(true);
        maybeStartMediaNegotiation();
        if (m_screenShareSession && !m_screenShareSession->setCameraSendingEnabled(false)) {
            setScreenSharing(false);
            emit infoMessage(QStringLiteral("Failed to stop camera sending"));
            return;
        }
        if (m_screenShareSession && !m_screenShareSession->setSharingEnabled(true)) {
            setScreenSharing(false);
            emit infoMessage(QStringLiteral("Failed to start screen sharing"));
            return;
        }
    } else {
        if (m_screenShareSession && !m_screenShareSession->setSharingEnabled(false)) {
            emit infoMessage(QStringLiteral("Failed to stop screen sharing"));
            return;
        }
        setScreenSharing(false);
        maybeStartMediaNegotiation();
        if (m_screenShareSession && !m_screenShareSession->setCameraSendingEnabled(!m_videoMuted)) {
            emit infoMessage(QStringLiteral("Failed to %1 camera sending")
                                 .arg(m_videoMuted ? QStringLiteral("stop") : QStringLiteral("start")));
            return;
        }
    }

    updateVideoSessionSettings();
    if (m_signaling->isConnected()) {
        m_signaling->sendMediaScreenShare(m_screenSharing);
    }
    syncLocalParticipantMediaState();
    if (!m_videoPeerUserId.isEmpty()) {
        m_videoOfferSentPeers.remove(m_videoPeerUserId);
        m_videoAnswerSentPeers.remove(m_videoPeerUserId);
    }
    sendVideoOfferToPeer(true);
}

bool MeetingController::setActiveShareUserId(const QString& userId) {
    const QString normalized = userId.trimmed();
    if (normalized.isEmpty() || normalized == m_userId) {
        return false;
    }

    QString nextDisplayName;
    bool found = false;
    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId != normalized || participant.userId == m_userId || !participant.sharing) {
            continue;
        }

        nextDisplayName = participant.displayName.trimmed().isEmpty() ? participant.userId : participant.displayName;
        found = true;
        break;
    }

    if (!found || (m_activeShareUserId == normalized && m_activeShareDisplayName == nextDisplayName)) {
        return found;
    }

    m_activeShareUserId = normalized;
    m_activeShareDisplayName = nextDisplayName;
    emit activeShareChanged();
    updateActiveVideoPeerSelection();

    maybeStartMediaNegotiation();
    sendVideoOfferToPeer(true);
    return true;
}

bool MeetingController::setActiveVideoPeerUserId(const QString& userId) {
    const QString normalized = userId.trimmed();
    if (normalized.isEmpty() || normalized == m_userId) {
        return false;
    }

    bool found = false;
    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId == normalized && participant.userId != m_userId && participant.videoOn) {
            found = true;
            break;
        }
    }

    if (!found) {
        return false;
    }

    if (m_videoPeerUserId == normalized && m_activeVideoPeerUserId == normalized) {
        return true;
    }

    m_videoPeerUserId = normalized;
    updateActiveVideoPeerSelection();

    maybeStartMediaNegotiation();
    sendVideoOfferToPeer(true);
    return true;
}

void MeetingController::setStatusText(const QString& text) {
    if (m_statusText == text) {
        return;
    }

    m_statusText = text;
    emit statusTextChanged();
}

void MeetingController::resetMeetingState(const QString& leaveReason) {
    resetMediaNegotiation();
    m_lastRouteStatusStage.clear();
    m_lastRouteStatusMessage.clear();
    m_lastRouteStatusAtMs = 0;
    if (!m_inMeeting && m_meetingId.isEmpty() && m_meetingTitle.isEmpty() && m_participantModel->rowCount() == 0) {
        m_waitingLeaveResponse = false;
        return;
    }

    const QString previousMeetingId = m_meetingId;
    const QString previousMeetingTitle = m_meetingTitle;
    const qint64 leftAt = QDateTime::currentMSecsSinceEpoch();
    if (!previousMeetingId.isEmpty()) {
        if (m_meetingRepository) {
            m_meetingRepository->markMeetingLeft(previousMeetingId, leftAt);
        }
        if (m_callLogRepository && m_currentMeetingJoinedAt > 0 && !m_userId.isEmpty()) {
            m_callLogRepository->finishActiveCall(previousMeetingId, m_userId, leaveReason, leftAt);
        }
    }

    const bool hadMeetingTitle = !previousMeetingTitle.isEmpty();
    m_inMeeting = false;
    m_audioMuted = false;
    m_videoMuted = shouldStartVideoMutedByDefault();
    setScreenSharing(false);
    m_waitingLeaveResponse = false;
    m_currentMeetingHost = false;
    m_currentMeetingJoinedAt = 0;
    m_meetingId.clear();
    m_meetingTitle.clear();
    m_sfuAddress.clear();
    m_iceServers.clear();
    m_participantModel->clearParticipants();
    if (m_localVideoFrameStore) {
        m_localVideoFrameStore->clear();
    }
    clearRemoteVideoFrameStores();
    syncParticipantsChanged();

    emit inMeetingChanged();
    emit audioMutedChanged();
    emit videoMutedChanged();
    emit localVideoFrameSourceChanged();
    emit meetingIdChanged();
    if (hadMeetingTitle) {
        emit meetingTitleChanged();
    }
}

void MeetingController::persistMeetingSessionStart(bool host) {
    if (m_meetingId.isEmpty()) {
        return;
    }

    const QString hostUserId = host ? m_userId : QString();
    if (m_currentMeetingJoinedAt > 0) {
        m_currentMeetingHost = m_currentMeetingHost || host;
        if (m_meetingRepository) {
            m_meetingRepository->upsertMeeting(m_meetingId, m_meetingTitle, m_currentMeetingHost ? m_userId : hostUserId, m_currentMeetingJoinedAt);
        }
        return;
    }

    const qint64 joinedAt = QDateTime::currentMSecsSinceEpoch();
    m_currentMeetingJoinedAt = joinedAt;
    m_currentMeetingHost = host;

    if (m_meetingRepository) {
        m_meetingRepository->upsertMeeting(m_meetingId, m_meetingTitle, hostUserId, joinedAt);
    }
    if (m_callLogRepository && !m_userId.isEmpty()) {
        m_callLogRepository->startCall(m_meetingId, m_meetingTitle, m_userId, joinedAt, host);
    }
}

void MeetingController::restoreCachedSession() {
    if (!m_userManager->hasCachedSession()) {
        return;
    }

    m_username = m_userManager->username();
    m_userId = m_userManager->userId();
    m_passwordHash = m_userManager->token();
    m_serverHost = m_userManager->serverHost();
    m_serverPort = m_userManager->serverPort();
    m_shouldStayConnected = true;
    m_waitingLogin = true;
    m_loginRequestSent = false;
    m_restoringSession = true;
    emit sessionChanged();

    setStatusText(QStringLiteral("Restoring session..."));
    m_signaling->connectToServer(m_serverHost, m_serverPort);
}

void MeetingController::handleProtobufMessage(quint16 signalType, const QByteArray& payload) {
    switch (signalType) {
    case kMeetStateSync: {
        meeting::MeetStateSyncNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetStateSyncNotify size=%1").arg(payload.size()));
            return;
        }

        QVector<ParticipantListModel::ParticipantItem> participants;
        participants.reserve(notify.participants_size());
        for (const auto& participant : notify.participants()) {
            participants.append(ParticipantListModel::fromProto(participant));
        }
        m_participantModel->replaceParticipants(participants);

        const QString meetingId = toQtString(notify.meeting_id());
        if (m_meetingId != meetingId) {
            m_meetingId = meetingId;
            emit meetingIdChanged();
        }

        const QString meetingTitle = toQtString(notify.title());
        if (m_meetingTitle != meetingTitle) {
            m_meetingTitle = meetingTitle;
            emit meetingTitleChanged();
        }

        m_waitingLeaveResponse = false;
        const QString hostId = toQtString(notify.host_id());
        m_participantModel->setHostUserId(hostId);
        m_currentMeetingHost = !hostId.isEmpty() && hostId == m_userId;
        ensureLocalParticipant(m_currentMeetingHost);
        for (const auto& participant : m_participantModel->items()) {
            if (participant.userId != m_userId) {
                continue;
            }

            const bool nextAudioMuted = !participant.audioOn;
            if (m_audioMuted != nextAudioMuted) {
                m_audioMuted = nextAudioMuted;
                emit audioMutedChanged();
            }

            const bool nextVideoMuted = !participant.videoOn;
            if (m_videoMuted != nextVideoMuted) {
                m_videoMuted = nextVideoMuted;
                emit videoMutedChanged();
            }

            if (m_audioCallSession) {
                m_audioCallSession->setCaptureMuted(m_audioMuted);
    (void)m_audioCallSession->setPreferredInputDeviceName(m_preferredMicrophoneDevice);
    (void)m_audioCallSession->setPreferredOutputDeviceName(m_preferredSpeakerDevice);
            }
            setScreenSharing(participant.sharing);
            if (m_screenShareSession) {
                m_screenShareSession->setSharingEnabled(m_screenSharing);
                m_screenShareSession->setCameraSendingEnabled(!m_screenSharing && !m_videoMuted);
            }
            updateAudioSessionSettings();
            updateVideoSessionSettings();
            break;
        }
        if (!m_inMeeting) {
            m_inMeeting = true;
            emit inMeetingChanged();
        }
        syncParticipantsChanged();
        setStatusText(QStringLiteral("Meeting state synced"));
        emit infoMessage(QStringLiteral("Meeting state synced"));
        return;
    }
    case kMeetParticipantJoin: {
        meeting::MeetParticipantJoinNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetParticipantJoinNotify"));
            return;
        }

        const auto participant = ParticipantListModel::fromProto(notify.participant());
        m_participantModel->upsertParticipant(participant.userId,
                                              participant.displayName,
                                              participant.avatarUrl,
                                              participant.role,
                                              participant.audioOn,
                                              participant.videoOn,
                                              participant.sharing,
                                              participant.audioSsrc,
                                              participant.videoSsrc);
        if (participant.role == 1) {
            m_participantModel->setHostUserId(participant.userId);
            m_currentMeetingHost = participant.userId == m_userId;
        }
        if (!m_inMeeting) {
            m_inMeeting = true;
            emit inMeetingChanged();
        }
        syncParticipantsChanged();
        emit infoMessage(QStringLiteral("%1 joined").arg(participant.displayName.isEmpty() ? participant.userId : participant.displayName));
        return;
    }
    case kMeetParticipantLeave: {
        meeting::MeetParticipantLeaveNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetParticipantLeaveNotify"));
            return;
        }

        const QString userId = toQtString(notify.user_id());
        const QString reason = toQtString(notify.reason());
        if (!m_userId.isEmpty() && userId == m_userId) {
            const QString msg = reason.isEmpty() ? QStringLiteral("You left the meeting") : reason;
            resetMeetingState(reason);
            setStatusText(msg);
            emit infoMessage(msg);
            return;
        }

        m_audioOfferSentPeers.remove(userId);
        m_audioAnswerSentPeers.remove(userId);
        m_videoOfferSentPeers.remove(userId);
        m_videoAnswerSentPeers.remove(userId);
        m_remoteVideoSsrcByPeer.remove(userId);
        if (m_audioPeerUserId == userId) {
            m_audioPeerUserId.clear();
        }
        if (m_videoPeerUserId == userId) {
            m_videoPeerUserId.clear();
        }
        m_participantModel->removeParticipant(userId);
        syncParticipantsChanged();
        emit infoMessage(reason.isEmpty() ? QStringLiteral("Participant left") : reason);
        return;
    }
    case kMeetHostChanged: {
        meeting::MeetHostChangedNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetHostChangedNotify"));
            return;
        }

        const QString newHostName = toQtString(notify.new_host_name());
        const QString newHostId = toQtString(notify.new_host_id());
        m_participantModel->setHostUserId(newHostId);
        m_currentMeetingHost = !newHostId.isEmpty() && newHostId == m_userId;
        syncParticipantsChanged();
        const QString msg = newHostName.isEmpty()
                                ? QStringLiteral("Host changed")
                                : QStringLiteral("Host changed to %1").arg(newHostName);
        setStatusText(msg);
        emit infoMessage(msg);
        return;
    }
    case kMeetMuteAll: {
        meeting::MeetMuteAllReq notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MeetMuteAllReq"));
            return;
        }
        if (!m_inMeeting) {
            return;
        }

        const bool muted = notify.mute();
        if (m_audioMuted != muted) {
            m_audioMuted = muted;
            emit audioMutedChanged();
        }
        if (m_audioCallSession) {
            m_audioCallSession->setCaptureMuted(m_audioMuted);
    (void)m_audioCallSession->setPreferredInputDeviceName(m_preferredMicrophoneDevice);
    (void)m_audioCallSession->setPreferredOutputDeviceName(m_preferredSpeakerDevice);
        }
        syncLocalParticipantMediaState();

        const QString msg = muted ? QStringLiteral("Host muted all") : QStringLiteral("Host unmuted all");
        setStatusText(msg);
        emit infoMessage(msg);
        return;
    }
    case kMediaRouteStatusNotify: {
        meeting::AuthKickNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit infoMessage(QStringLiteral("Failed to parse MediaRouteStatusNotify"));
            return;
        }

        const MediaRouteStatusEvent event = parseMediaRouteStatusEvent(toQtString(notify.reason()));
        if (event.message.isEmpty()) {
            return;
        }

        setStatusText(event.message);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool duplicate = (m_lastRouteStatusStage == event.stage && m_lastRouteStatusMessage == event.message);
        const qint64 elapsedMs = m_lastRouteStatusAtMs > 0 ? (nowMs - m_lastRouteStatusAtMs) : std::numeric_limits<qint64>::max();

        m_lastRouteStatusStage = event.stage;
        m_lastRouteStatusMessage = event.message;
        m_lastRouteStatusAtMs = nowMs;

        // "switching" is shown in status text only; toast-like info messages are reserved
        // for terminal states to avoid noisy UI hints under repeated retries.
        bool shouldEmitInfo = false;
        if (event.stage == QStringLiteral("failed") || event.stage == QStringLiteral("switched")) {
            shouldEmitInfo = !duplicate || elapsedMs > 3000;
        } else if (event.stage == QStringLiteral("info")) {
            shouldEmitInfo = !duplicate || elapsedMs > 3000;
        }
        if (shouldEmitInfo) {
            emit infoMessage(event.message);
        }
        if (event.stage == QStringLiteral("switched") && !event.route.isEmpty()) {
            updateSfuRoute(event.route);
            if (m_inMeeting && m_signaling && m_signaling->isConnected()) {
                (void)sendAudioOfferToPeer(true);
                (void)sendVideoOfferToPeer(true);
            }
        }
        return;
    }
    default:
        return;
    }
}

void MeetingController::ensureLocalParticipant(bool host) {
    if (m_userId.isEmpty()) {
        return;
    }

    ParticipantListModel::ParticipantItem existingItem;
    bool hasExistingItem = false;
    for (const auto& item : m_participantModel->items()) {
        if (item.userId == m_userId) {
            existingItem = item;
            hasExistingItem = true;
            break;
        }
    }

    const QString displayName = hasExistingItem
                                    ? existingItem.displayName
                                    : (m_username.isEmpty() ? QStringLiteral("You") : m_username);
    if (!hasExistingItem) {
        m_participantModel->upsertParticipant(m_userId,
                                              displayName,
                                              QString(),
                                              host ? 1 : 0,
                                              !m_audioMuted,
                                              !m_videoMuted,
                                              m_screenSharing,
                                              m_assignedAudioSsrc,
                                              m_assignedVideoSsrc);
    } else if (host && existingItem.role != 1) {
        m_participantModel->upsertParticipant(m_userId,
                                              displayName,
                                              existingItem.avatarUrl,
                                              1,
                                              existingItem.audioOn,
                                              existingItem.videoOn,
                                              existingItem.sharing,
                                              m_assignedAudioSsrc != 0U ? m_assignedAudioSsrc : existingItem.audioSsrc,
                                              m_assignedVideoSsrc != 0U ? m_assignedVideoSsrc : existingItem.videoSsrc);
    }

    if (host) {
        m_participantModel->setHostUserId(m_userId);
    }
}

void MeetingController::syncParticipantsChanged() {
    updateRemoteVideoSsrcMappings();
    prunePeerNegotiationState();
    const QString previousActiveShareUserId = m_activeShareUserId;
    emit participantsChanged();
    updateActiveShareSelection();
    updateActiveVideoPeerSelection();
    maybeStartMediaNegotiation();
    if (previousActiveShareUserId != m_activeShareUserId) {
        sendVideoOfferToPeer(true);
    }
}

void MeetingController::updateRemoteVideoSsrcMappings() {
    QHash<QString, quint32> next;

    int roomVideoPublisherCount = 0;
    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId.isEmpty() || participant.videoSsrc == 0U ||
            (!participant.videoOn && !participant.sharing)) {
            continue;
        }
        ++roomVideoPublisherCount;
    }

    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId.isEmpty() || participant.userId == m_userId || participant.videoSsrc == 0U ||
            (!participant.videoOn && !participant.sharing)) {
            continue;
        }
        quint32 remoteVideoSsrc = 0U;
        if (roomVideoPublisherCount > 1 && m_assignedVideoSsrc != 0U) {
            remoteVideoSsrc = routeRewriteSsrc(m_meetingId, m_userId, participant.videoSsrc, true);
            if (remoteVideoSsrc == m_assignedVideoSsrc || remoteVideoSsrc == participant.videoSsrc) {
                remoteVideoSsrc ^= 0x01010101U;
            }
            if (remoteVideoSsrc == 0U) {
                remoteVideoSsrc = participant.videoSsrc ^ 0x01010101U;
            }
        } else if (m_assignedVideoSsrc != 0U) {
            remoteVideoSsrc = m_assignedVideoSsrc;
        } else {
            remoteVideoSsrc = participant.videoSsrc;
        }
        if (remoteVideoSsrc != 0U) {
            next.insert(participant.userId, remoteVideoSsrc);
        }
    }

    if (m_remoteVideoSsrcByPeer == next) {
        return;
    }

    m_remoteVideoSsrcByPeer = next;
    updateExpectedRemoteVideoSsrcForCurrentPeer();
    emit remoteVideoFrameSourceChanged();
}

bool MeetingController::hasRemoteParticipant(const QString& userId) const {
    if (userId.isEmpty() || userId == m_userId) {
        return false;
    }
    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId == userId && participant.userId != m_userId) {
            return true;
        }
    }
    return false;
}

bool MeetingController::hasRemoteVideoParticipant(const QString& userId) const {
    if (userId.isEmpty() || userId == m_userId) {
        return false;
    }

    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId != userId || participant.userId == m_userId) {
            continue;
        }
        return participant.videoOn || participant.sharing;
    }

    return false;
}

void MeetingController::prunePeerNegotiationState() {
    QSet<QString> remoteUsers;
    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId.isEmpty() || participant.userId == m_userId) {
            continue;
        }
        remoteUsers.insert(participant.userId);
    }

    auto pruneSet = [&remoteUsers](QSet<QString>& peers) {
        for (auto it = peers.begin(); it != peers.end();) {
            if (*it != QLatin1String(kSfuTransportKey) && !remoteUsers.contains(*it)) {
                it = peers.erase(it);
            } else {
                ++it;
            }
        }
    };

    pruneSet(m_audioOfferSentPeers);
    pruneSet(m_audioAnswerSentPeers);
    pruneSet(m_videoOfferSentPeers);
    pruneSet(m_videoAnswerSentPeers);

    for (auto it = m_remoteVideoSsrcByPeer.begin(); it != m_remoteVideoSsrcByPeer.end();) {
        if (!remoteUsers.contains(it.key())) {
            it = m_remoteVideoSsrcByPeer.erase(it);
        } else {
            ++it;
        }
    }

    bool removedFrameStore = false;
    for (auto it = m_remoteVideoFrameStoresByPeer.begin(); it != m_remoteVideoFrameStoresByPeer.end();) {
        const bool keepStore = remoteUsers.contains(it.key()) && hasRemoteVideoParticipant(it.key());
        if (keepStore) {
            ++it;
            continue;
        }
        if (it.value() != nullptr) {
            it.value()->clear();
            it.value()->deleteLater();
        }
        it = m_remoteVideoFrameStoresByPeer.erase(it);
        removedFrameStore = true;
    }
    if (removedFrameStore) {
        emit remoteVideoFrameSourceChanged();
    }

    if (!m_audioPeerUserId.isEmpty() && !remoteUsers.contains(m_audioPeerUserId)) {
        m_audioPeerUserId.clear();
    }
    if (!m_videoPeerUserId.isEmpty() && !remoteUsers.contains(m_videoPeerUserId)) {
        m_videoPeerUserId.clear();
    }

    updateExpectedRemoteVideoSsrcForCurrentPeer();
}

void MeetingController::handleSessionKicked(const QString& reason) {
    const QString msg = reason.isEmpty() ? QStringLiteral("You were signed out") : reason;

    m_shouldStayConnected = false;
    m_waitingLogin = false;
    m_loginRequestSent = false;
    m_restoringSession = false;
    m_waitingLeaveResponse = false;
    m_pendingMeetingTitle.clear();
    m_reconnector->stop();
    m_heartbeatTimer->stop();
    resetMeetingState(msg);
    m_passwordHash.clear();
    m_userId.clear();
    m_userManager->clearToken();
    emit sessionChanged();

    if (m_reconnecting) {
        m_reconnecting = false;
        emit reconnectingChanged();
    }

    if (m_loggedIn) {
        m_loggedIn = false;
        emit loggedInChanged();
    }

    m_signaling->disconnectFromServer();
    setStatusText(msg);
    emit infoMessage(msg);
}


















QString MeetingController::currentAudioPeerUserId() const {
    if (m_userId.isEmpty()) {
        return {};
    }

    if (hasRemoteParticipant(m_audioPeerUserId)) {
        return m_audioPeerUserId;
    }

    QString firstRemotePeerUserId;
    const auto participants = m_participantModel->items();
    for (const auto& participant : participants) {
        if (participant.userId.isEmpty() || participant.userId == m_userId) {
            continue;
        }

        if (participant.host) {
            return participant.userId;
        }
        if (firstRemotePeerUserId.isEmpty()) {
            firstRemotePeerUserId = participant.userId;
        }
    }

    return firstRemotePeerUserId;
}

QString MeetingController::currentVideoPeerUserId() const {
    if (!m_activeShareUserId.isEmpty()) {
        return m_activeShareUserId;
    }

    if (hasRemoteVideoParticipant(m_videoPeerUserId)) {
        return m_videoPeerUserId;
    }

    if (m_screenSharing) {
        return currentAudioPeerUserId();
    }

    for (const auto& participant : m_participantModel->items()) {
        if (participant.userId.isEmpty() || participant.userId == m_userId) {
            continue;
        }
        if (participant.sharing || participant.videoOn) {
            return participant.userId;
        }
    }

    return currentAudioPeerUserId();
}

QString MeetingController::currentAudioTransportKey() const {
    return resolveSfuEndpoint(nullptr, nullptr) ? QString::fromLatin1(kSfuTransportKey)
                                                : currentAudioPeerUserId();
}

QString MeetingController::currentVideoTransportKey() const {
    return resolveSfuEndpoint(nullptr, nullptr) ? QString::fromLatin1(kSfuTransportKey)
                                                : currentVideoPeerUserId();
}

quint32 MeetingController::currentVideoSsrc() const {
    if (!m_screenShareSession) {
        return 0;
    }

    if (m_screenShareSession->sharingEnabled() || m_screenShareSession->cameraSendingEnabled()) {
        return m_screenShareSession->videoSsrc();
    }

    return 0;
}

QString MeetingController::resolvePeerUserIdForRemoteVideoSsrc(quint32 remoteSsrc) const {
    if (remoteSsrc != 0U) {
        for (auto it = m_remoteVideoSsrcByPeer.constBegin(); it != m_remoteVideoSsrcByPeer.constEnd(); ++it) {
            if (it.value() == remoteSsrc && hasRemoteVideoParticipant(it.key())) {
                return it.key();
            }
        }
        for (auto it = m_remoteVideoSsrcByPeer.constBegin(); it != m_remoteVideoSsrcByPeer.constEnd(); ++it) {
            if (it.value() == remoteSsrc) {
                return it.key();
            }
        }
    }

    const QString configuredPeerUserId = m_videoPeerUserId.trimmed();
    if (!configuredPeerUserId.isEmpty()) {
        return configuredPeerUserId;
    }
    return currentVideoPeerUserId();
}

void MeetingController::updateExpectedRemoteVideoSsrcForCurrentPeer() {
    if (!m_screenShareSession) {
        return;
    }

    int activeVideoSsrcCount = 0;
    for (auto it = m_remoteVideoSsrcByPeer.constBegin(); it != m_remoteVideoSsrcByPeer.constEnd(); ++it) {
        if (it.value() == 0U || !hasRemoteVideoParticipant(it.key())) {
            continue;
        }
        ++activeVideoSsrcCount;
        if (activeVideoSsrcCount > 1) {
            m_screenShareSession->setExpectedRemoteVideoSsrc(0U);
            return;
        }
    }

    const QString peerUserId = currentVideoPeerUserId();
    m_screenShareSession->setExpectedRemoteVideoSsrc(remoteVideoSsrcForUser(peerUserId));
}
void MeetingController::updateAudioSessionSettings() {
    if (!m_audioSessionManager) {
        return;
    }

    m_audioSessionManager->setLocalUserId(m_userId);
    m_audioSessionManager->setMeetingId(m_meetingId);
    m_audioSessionManager->setLocalHost(resolveAdvertisedHost());
    m_audioSessionManager->setAudioPayloadType(kAudioPayloadType);
    m_audioSessionManager->setVideoPayloadType(kScreenPayloadType);
    m_audioSessionManager->setAudioNegotiationEnabled(true);
    m_audioSessionManager->setVideoNegotiationEnabled(false);
    if (m_audioCallSession) {
        m_audioSessionManager->setLocalAudioSsrc(m_audioCallSession->audioSsrc());
        const quint16 localPort = m_audioCallSession->localPort();
        if (localPort != 0) {
            m_audioSessionManager->setLocalAudioPort(localPort);
        }
    } else {
        m_audioSessionManager->setLocalAudioSsrc(0);
    }
    m_audioSessionManager->setLocalVideoSsrc(0);
}

void MeetingController::updateSfuRoute(const QString& route) {
    const QString normalized = route.trimmed();
    if (m_sfuAddress == normalized) {
        return;
    }

    m_sfuAddress = normalized;
    applySfuRouteToSessions();
}

bool MeetingController::resolveSfuEndpoint(QString* host, quint16* port) const {
    QString parsedHost;
    quint16 parsedPort = 0;
    if (!parseIpv4Endpoint(m_sfuAddress, &parsedHost, &parsedPort)) {
        return false;
    }

    if (host != nullptr) {
        *host = parsedHost;
    }
    if (port != nullptr) {
        *port = parsedPort;
    }
    return true;
}

void MeetingController::applySfuRouteToSessions() {
    if (icePolicyFromEnvironment() == IcePolicy::RelayOnly) {
        return;
    }

    QString routeHost;
    quint16 routePort = 0;
    if (!resolveSfuEndpoint(&routeHost, &routePort)) {
        return;
    }

    if (m_audioCallSession) {
        m_audioCallSession->setPeer(routeHost.toStdString(), routePort);
    }
    if (m_screenShareSession) {
        m_screenShareSession->setPeer(routeHost.toStdString(), routePort);
    }
}

void MeetingController::handleMediaTransportAnswer(const QString& meetingId,
                                                   const QString& serverIceUfrag,
                                                   const QString& serverIcePwd,
                                                   const QString& serverDtlsFingerprint,
                                                   const QStringList& serverCandidates,
                                                   quint32 assignedAudioSsrc,
                                                   quint32 assignedVideoSsrc) {
    if (!m_inMeeting || meetingId.trimmed() != m_meetingId) {
        return;
    }
    if (serverIceUfrag.trimmed().isEmpty() ||
        serverIcePwd.trimmed().isEmpty() ||
        serverDtlsFingerprint.trimmed().isEmpty() ||
        serverCandidates.isEmpty()) {
        emit infoMessage(QStringLiteral("Media transport answer rejected: incomplete SFU transport parameters"));
        return;
    }

    if (assignedAudioSsrc != 0U) {
        m_assignedAudioSsrc = assignedAudioSsrc;
        if (m_audioCallSession) {
            m_audioCallSession->setAudioSsrc(assignedAudioSsrc);
        }
        if (m_audioSessionManager) {
            m_audioSessionManager->setLocalAudioSsrc(assignedAudioSsrc);
        }
    }

    if (assignedVideoSsrc != 0U) {
        m_assignedVideoSsrc = assignedVideoSsrc;
        if (m_screenShareSession) {
            m_screenShareSession->setVideoSsrc(assignedVideoSsrc);
            m_screenShareSession->setExpectedRemoteVideoSsrc(0U);
        }
        if (m_mediaSessionManager) {
            m_mediaSessionManager->setLocalVideoSsrc(assignedVideoSsrc);
        }
    }
    const bool relayOnly = icePolicyFromEnvironment() == IcePolicy::RelayOnly;
    QString routeHost;
    quint16 routePort = 0;
    if (!resolveSfuEndpoint(&routeHost, &routePort)) {
        emit infoMessage(QStringLiteral("Media transport answer rejected: invalid SFU route"));
        return;
    }

    updateRemoteVideoSsrcMappings();

    if (!relayOnly) {
        applySfuRouteToSessions();
    }

    TurnRelaySettings turnRelay;
    if (relayOnly) {
        turnRelay = firstTurnRelaySettings(m_iceServers);
        if (!turnRelay.valid) {
            emit infoMessage(QStringLiteral("TURN relay requested but no usable TURN server was provided"));
            return;
        }
    }

    const bool answerForAudio = assignedAudioSsrc != 0U ||
                                (assignedAudioSsrc == 0U && assignedVideoSsrc == 0U);
    const bool answerForVideo = assignedVideoSsrc != 0U;

    if (answerForAudio && m_audioCallSession && !m_audioClientIceUfrag.isEmpty()) {
        if (relayOnly &&
            !m_audioCallSession->configureTurnRelay(turnRelay.host.toStdString(),
                                                    turnRelay.port,
                                                    turnRelay.username.toStdString(),
                                                    turnRelay.credential.toStdString(),
                                                    routeHost.toStdString(),
                                                    routePort)) {
            emit infoMessage(QStringLiteral("Audio TURN relay configure failed: %1")
                                 .arg(QString::fromStdString(m_audioCallSession->lastError())));
            return;
        }
        const auto stunRequest = buildStunBindingRequest(serverIceUfrag + QLatin1Char(':') + m_audioClientIceUfrag);
        if (!m_audioCallSession->sendTransportProbe(stunRequest) && shouldEmitVerboseNegotiationLogs()) {
            emit infoMessage(QStringLiteral("Audio ICE binding request send failed"));
        }
        if (!m_audioCallSession->startDtlsSrtp(serverDtlsFingerprint) && shouldEmitVerboseNegotiationLogs()) {
            emit infoMessage(QStringLiteral("Audio DTLS-SRTP start failed: %1")
                                 .arg(QString::fromStdString(m_audioCallSession->lastError())));
        }
    }
    if (answerForVideo && m_screenShareSession && !m_videoClientIceUfrag.isEmpty()) {
        if (relayOnly &&
            !m_screenShareSession->configureTurnRelay(turnRelay.host.toStdString(),
                                                      turnRelay.port,
                                                      turnRelay.username.toStdString(),
                                                      turnRelay.credential.toStdString(),
                                                      routeHost.toStdString(),
                                                      routePort)) {
            emit infoMessage(QStringLiteral("Video TURN relay configure failed: %1")
                                 .arg(QString::fromStdString(m_screenShareSession->lastError())));
            return;
        }
        const auto stunRequest = buildStunBindingRequest(serverIceUfrag + QLatin1Char(':') + m_videoClientIceUfrag);
        if (!m_screenShareSession->sendTransportProbe(stunRequest) && shouldEmitVerboseNegotiationLogs()) {
            emit infoMessage(QStringLiteral("Video ICE binding request send failed"));
        }
        if (!m_screenShareSession->startDtlsSrtp(serverDtlsFingerprint) && shouldEmitVerboseNegotiationLogs()) {
            emit infoMessage(QStringLiteral("Video DTLS-SRTP start failed: %1")
                                 .arg(QString::fromStdString(m_screenShareSession->lastError())));
        }
    }
    updateAudioSessionSettings();
    updateVideoSessionSettings();

    if (shouldEmitVerboseNegotiationLogs()) {
        emit infoMessage(QStringLiteral("Media transport answer accepted: sfu-candidates=%1 assigned-audio-ssrc=%2 assigned-video-ssrc=%3")
                             .arg(serverCandidates.size())
                             .arg(assignedAudioSsrc)
                             .arg(assignedVideoSsrc));
    }
}

void MeetingController::updateVideoSessionSettings() {
    if (!m_mediaSessionManager) {
        return;
    }

    m_mediaSessionManager->setLocalUserId(m_userId);
    m_mediaSessionManager->setMeetingId(m_meetingId);
    m_mediaSessionManager->setLocalHost(resolveAdvertisedHost());
    m_mediaSessionManager->setAudioPayloadType(kAudioPayloadType);
    m_mediaSessionManager->setVideoPayloadType(m_screenSharing ? kScreenPayloadType : kCameraPayloadType);
    m_mediaSessionManager->setAudioNegotiationEnabled(false);
    m_mediaSessionManager->setVideoNegotiationEnabled(true);
    m_mediaSessionManager->setLocalAudioSsrc(0);
    if (m_screenShareSession && m_screenShareSession->isRunning()) {
        const bool enableSharing = m_screenSharing;
        const bool enableCamera = !m_screenSharing && !m_videoMuted;

        if (enableSharing) {
            if (m_screenShareSession->cameraSendingEnabled()) {
                if (!m_screenShareSession->setCameraSendingEnabled(false)) {
                    emit infoMessage(QStringLiteral("Failed to stop camera sending: %1")
                                         .arg(QString::fromStdString(m_screenShareSession->lastError())));
                }
            }
            if (!m_screenShareSession->sharingEnabled()) {
                if (!m_screenShareSession->setSharingEnabled(true)) {
                    emit infoMessage(QStringLiteral("Failed to enable screen sharing: %1")
                                         .arg(QString::fromStdString(m_screenShareSession->lastError())));
                }
            }
        } else {
            if (m_screenShareSession->sharingEnabled()) {
                if (!m_screenShareSession->setSharingEnabled(false)) {
                    emit infoMessage(QStringLiteral("Failed to disable screen sharing: %1")
                                         .arg(QString::fromStdString(m_screenShareSession->lastError())));
                }
            }
            if (m_screenShareSession->cameraSendingEnabled() != enableCamera) {
                if (!m_screenShareSession->setCameraSendingEnabled(enableCamera)) {
                    emit infoMessage(QStringLiteral("Failed to %1 camera sending: %2")
                                         .arg(enableCamera ? QStringLiteral("start") : QStringLiteral("stop"),
                                              QString::fromStdString(m_screenShareSession->lastError())));
                }
            }
        }

        const quint16 videoPort = m_screenShareSession->localPort();
        if (videoPort != 0) {
            m_mediaSessionManager->setLocalVideoPort(videoPort);
        }
        updateExpectedRemoteVideoSsrcForCurrentPeer();
        m_mediaSessionManager->setLocalVideoSsrc(currentVideoSsrc());
    } else {
        m_mediaSessionManager->setLocalVideoSsrc(0);
    }
}

void MeetingController::updateActiveShareSelection() {
    QString nextUserId;
    QString nextDisplayName;

    const auto participants = m_participantModel->items();
    for (const auto& participant : participants) {
        if (participant.userId == m_activeShareUserId && participant.userId != m_userId && participant.sharing) {
            nextUserId = participant.userId;
            nextDisplayName = participant.displayName.trimmed().isEmpty() ? participant.userId : participant.displayName;
            break;
        }
    }

    if (nextUserId.isEmpty()) {
        for (const auto& participant : participants) {
            if (participant.userId.isEmpty() || participant.userId == m_userId || !participant.sharing) {
                continue;
            }
            nextUserId = participant.userId;
            nextDisplayName = participant.displayName.trimmed().isEmpty() ? participant.userId : participant.displayName;
            break;
        }
    }

    if (m_activeShareUserId == nextUserId && m_activeShareDisplayName == nextDisplayName) {
        return;
    }

    m_activeShareUserId = nextUserId;
    m_activeShareDisplayName = nextDisplayName;
    emit activeShareChanged();
}

void MeetingController::updateActiveVideoPeerSelection() {
    const bool hadActiveShare = hasActiveShare();
    const QString previousUserId = m_activeVideoPeerUserId;
    const QString nextUserId = currentVideoPeerUserId();
    if (m_activeVideoPeerUserId == nextUserId) {
        return;
    }

    m_activeVideoPeerUserId = nextUserId;
    qInfo().noquote() << "[meeting] active video peer=" << (m_activeVideoPeerUserId.isEmpty() ? QStringLiteral("<none>") : m_activeVideoPeerUserId)
                      << "share=" << hasActiveShare();
    updateExpectedRemoteVideoSsrcForCurrentPeer();
    emit activeVideoPeerUserIdChanged();
    emit remoteVideoFrameSourceChanged();
    if (!hadActiveShare && previousUserId != nextUserId && m_remoteVideoFrameStore) {
        invalidateRemoteVideoRenderQueue(true);
    }
}

void MeetingController::enqueueRemoteVideoRenderTask(std::function<void()> renderTask,
                                                     int renderDelayMs,
                                                     int64_t videoPts90k,
                                                     const QString& peerUserId) {
    if (!renderTask || !m_videoRenderTimer || !m_remoteVideoFrameStore) {
        return;
    }

    const auto& tuning = videoRenderTuning();

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    PendingVideoRenderTask task;
    task.dueAtMs = nowMs + (renderDelayMs > 0 ? renderDelayMs : 0);
    task.audioDelayDeadlineMs = tuning.maxAudioDrivenRenderDelayMs > 0
                                  ? task.dueAtMs + tuning.maxAudioDrivenRenderDelayMs
                                  : 0;
    task.videoPts90k = videoPts90k;
    task.enqueueSeq = ++m_videoRenderEnqueueSeq;
    task.ticket = m_videoRenderTicket;
    task.peerUserId = peerUserId;
    task.render = std::move(renderTask);
    ++m_remoteVideoQueuedFrameCount;

    auto insertIt = m_remoteVideoRenderQueue.end();
    for (auto it = m_remoteVideoRenderQueue.begin(); it != m_remoteVideoRenderQueue.end(); ++it) {
        if (task.dueAtMs < it->dueAtMs ||
            (task.dueAtMs == it->dueAtMs && task.enqueueSeq < it->enqueueSeq)) {
            insertIt = it;
            break;
        }
    }
    m_remoteVideoRenderQueue.insert(insertIt, std::move(task));

    if (!peerUserId.isEmpty()) {
        int peerTaskCount = 0;
        for (const auto& queuedTask : m_remoteVideoRenderQueue) {
            if (queuedTask.peerUserId == peerUserId) {
                ++peerTaskCount;
            }
        }
        while (peerTaskCount > tuning.maxPerPeerQueueDepth) {
            auto eraseIt = m_remoteVideoRenderQueue.end();
            for (auto it = m_remoteVideoRenderQueue.begin(); it != m_remoteVideoRenderQueue.end(); ++it) {
                if (it->peerUserId == peerUserId) {
                    eraseIt = it;
                    break;
                }
            }
            if (eraseIt == m_remoteVideoRenderQueue.end()) {
                break;
            }
            m_remoteVideoRenderQueue.erase(eraseIt);
            --peerTaskCount;
        }
    }

    while (static_cast<int>(m_remoteVideoRenderQueue.size()) > tuning.maxRemoteQueueDepth) {
        m_remoteVideoRenderQueue.pop_front();
    }

    if (!m_videoRenderTimer->isActive()) {
        m_videoRenderTimer->start();
    }
}

void MeetingController::drainRemoteVideoRenderQueue() {
    if (!m_videoRenderTimer) {
        return;
    }

    const auto& tuning = videoRenderTuning();

    if (m_remoteVideoRenderQueue.empty()) {
        m_videoRenderTimer->stop();
        return;
    }

    if (!m_remoteVideoFrameStore || hasActiveShare()) {
        invalidateRemoteVideoRenderQueue(false);
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    std::vector<PendingVideoRenderTask> dueTasks;
    while (!m_remoteVideoRenderQueue.empty()) {
        const auto& front = m_remoteVideoRenderQueue.front();
        if (front.dueAtMs > nowMs) {
            break;
        }

        dueTasks.push_back(std::move(m_remoteVideoRenderQueue.front()));
        m_remoteVideoRenderQueue.pop_front();
    }

    std::stable_sort(dueTasks.begin(), dueTasks.end(),
                     [](const PendingVideoRenderTask& lhs, const PendingVideoRenderTask& rhs) {
                         const bool lhsValidPts = lhs.videoPts90k >= 0;
                         const bool rhsValidPts = rhs.videoPts90k >= 0;
                         if (lhsValidPts != rhsValidPts) {
                             return lhsValidPts;
                         }
                         if (lhsValidPts && lhs.videoPts90k != rhs.videoPts90k) {
                             return lhs.videoPts90k < rhs.videoPts90k;
                         }
                         if (lhs.dueAtMs != rhs.dueAtMs) {
                             return lhs.dueAtMs < rhs.dueAtMs;
                         }
                         return lhs.enqueueSeq < rhs.enqueueSeq;
                     });

    // Keep render latency bounded by dropping older overdue frames when backlog spikes.
    if (static_cast<int>(dueTasks.size()) > tuning.maxVideoFramesPerDrain) {
        dueTasks.erase(dueTasks.begin(), dueTasks.end() - tuning.maxVideoFramesPerDrain);
    }

    auto insertTask = [this](PendingVideoRenderTask&& task) {
        auto insertIt = m_remoteVideoRenderQueue.end();
        for (auto it = m_remoteVideoRenderQueue.begin(); it != m_remoteVideoRenderQueue.end(); ++it) {
            if (task.dueAtMs < it->dueAtMs ||
                (task.dueAtMs == it->dueAtMs && task.enqueueSeq < it->enqueueSeq)) {
                insertIt = it;
                break;
            }
        }
        m_remoteVideoRenderQueue.insert(insertIt, std::move(task));
    };

    const auto clock = m_audioCallSession ? m_audioCallSession->clock() : std::shared_ptr<av::sync::AVSync>{};
    std::vector<PendingVideoRenderTask> rescheduledTasks;
    for (auto& task : dueTasks) {
        if (task.ticket != m_videoRenderTicket || !task.render) {
            continue;
        }

        if (task.videoPts90k >= 0 &&
            m_lastRenderedVideoPts90k >= 0 &&
            task.videoPts90k <= m_lastRenderedVideoPts90k) {
            ++m_remoteVideoStalePtsDropCount;
            continue;
        }

        const qint64 renderCheckMs = QDateTime::currentMSecsSinceEpoch();

        if (clock && task.videoPts90k >= 0) {
            const int extraDelayMs = av::sync::AVSync::suggestVideoRenderDelayMsByAudioClock(task.videoPts90k,
                                                                                               clock->audioPts(),
                                                                                               clock->sampleRate(),
                                                                                               tuning.maxAudioDrivenRenderDelayMs);
            if (extraDelayMs > 0) {
                const qint64 candidateDueAtMs = renderCheckMs + extraDelayMs;
                if (task.audioDelayDeadlineMs > 0 && candidateDueAtMs >= task.audioDelayDeadlineMs) {
                    task.dueAtMs = task.audioDelayDeadlineMs;
                } else {
                    task.dueAtMs = candidateDueAtMs;
                    task.enqueueSeq = ++m_videoRenderEnqueueSeq;
                    ++m_remoteVideoRescheduledFrameCount;
                    rescheduledTasks.push_back(std::move(task));
                    continue;
                }
            }
        }

        if (task.videoPts90k >= 0 &&
            m_lastRenderedVideoPts90k >= 0 &&
            m_lastVideoRenderAtMs > 0) {
            const int64_t currentVideoMs = av::sync::AVSync::videoPts90kToTimeMs(task.videoPts90k);
            const int64_t previousVideoMs = av::sync::AVSync::videoPts90kToTimeMs(m_lastRenderedVideoPts90k);
            if (currentVideoMs >= 0 && previousVideoMs >= 0 && currentVideoMs > previousVideoMs) {
                int cadenceMs = static_cast<int>(currentVideoMs - previousVideoMs);
                cadenceMs = std::clamp(cadenceMs, tuning.minVideoCadenceMs, tuning.maxVideoCadenceMs);
                const qint64 sinceLastRenderMs = renderCheckMs - m_lastVideoRenderAtMs;
                if (sinceLastRenderMs < cadenceMs) {
                    task.dueAtMs = renderCheckMs + (cadenceMs - static_cast<int>(sinceLastRenderMs));
                    task.enqueueSeq = ++m_videoRenderEnqueueSeq;
                    ++m_remoteVideoRescheduledFrameCount;
                    rescheduledTasks.push_back(std::move(task));
                    continue;
                }
            }
        }

        if (task.videoPts90k < 0) {
            ++m_videoAudioSkewInvalidVideoPtsCount;
        } else if (!clock) {
            ++m_videoAudioSkewNoClockCount;
        }

        if (clock && task.videoPts90k >= 0) {
            ++m_videoAudioSkewCandidateCount;
            const int64_t videoMs = av::sync::AVSync::videoPts90kToTimeMs(task.videoPts90k);
            const int64_t audioMs = av::sync::AVSync::audioPtsToTimeMs(clock->audioPts(), clock->sampleRate());
            if (videoMs < 0) {
                ++m_videoAudioSkewInvalidVideoPtsCount;
            } else if (audioMs < 0) {
                ++m_videoAudioSkewInvalidAudioClockCount;
            } else {
                if (!m_videoAudioSkewBaselineReady) {
                    m_videoAudioSkewBaselineVideoMs = static_cast<qint64>(videoMs);
                    m_videoAudioSkewBaselineAudioMs = static_cast<qint64>(audioMs);
                    m_videoAudioSkewBaselineReady = true;
                }
                const int64_t alignedVideoMs = videoMs - m_videoAudioSkewBaselineVideoMs;
                const int64_t alignedAudioMs = audioMs - m_videoAudioSkewBaselineAudioMs;
                const int64_t skewMs = alignedVideoMs - alignedAudioMs;
                m_hasVideoAudioSkewSample = true;
                m_lastVideoAudioSkewMs = static_cast<qint64>(skewMs);
                const qint64 absSkewMs = static_cast<qint64>(std::llabs(skewMs));
                if (absSkewMs > m_maxAbsVideoAudioSkewMs) {
                    m_maxAbsVideoAudioSkewMs = absSkewMs;
                }
                ++m_videoAudioSkewSampleCount;
            }
        }

        task.render();
        ++m_remoteVideoRenderedFrameCount;
        m_lastVideoRenderAtMs = QDateTime::currentMSecsSinceEpoch();
        if (task.videoPts90k >= 0) {
            m_lastRenderedVideoPts90k = task.videoPts90k;
        }
    }

    for (auto& task : rescheduledTasks) {
        insertTask(std::move(task));
    }

    while (static_cast<int>(m_remoteVideoRenderQueue.size()) > tuning.maxRemoteQueueDepth) {
        m_remoteVideoRenderQueue.pop_front();
    }

    if (m_remoteVideoRenderQueue.empty()) {
        m_videoRenderTimer->stop();
        return;
    }

    if (!m_videoRenderTimer->isActive()) {
        m_videoRenderTimer->start();
    }
}
void MeetingController::invalidateRemoteVideoRenderQueue(bool clearFrameStore) {
    ++m_remoteVideoQueueResetCount;
    ++m_videoRenderTicket;
    m_remoteVideoRenderQueue.clear();
    m_lastVideoRenderAtMs = 0;
    m_lastRenderedVideoPts90k = -1;

    if (m_videoRenderTimer) {
        m_videoRenderTimer->stop();
    }

    if (clearFrameStore && m_remoteVideoFrameStore) {
        m_remoteVideoFrameStore->clear();
    }
}

void MeetingController::resetRemoteVideoAvSyncStats() {
    m_lastVideoAudioSkewMs = 0;
    m_maxAbsVideoAudioSkewMs = 0;
    m_videoAudioSkewSampleCount = 0;
    m_hasVideoAudioSkewSample = false;
    m_videoAudioSkewBaselineReady = false;
    m_videoAudioSkewBaselineVideoMs = 0;
    m_videoAudioSkewBaselineAudioMs = 0;
    m_videoAudioSkewCandidateCount = 0;
    m_videoAudioSkewNoClockCount = 0;
    m_videoAudioSkewInvalidVideoPtsCount = 0;
    m_videoAudioSkewInvalidAudioClockCount = 0;
}

void MeetingController::resetAudioPeerState() {
    if (!m_audioTransportKey.isEmpty()) {
        m_audioOfferSentPeers.remove(m_audioTransportKey);
        m_audioAnswerSentPeers.remove(m_audioTransportKey);
    }
    m_audioNegotiationStarted = false;
    m_audioPeerUserId.clear();
    m_audioTransportKey.clear();

    if (m_audioSessionManager) {
        m_audioSessionManager->reset();
    }
}

av::render::VideoFrameStore* MeetingController::ensureRemoteVideoFrameStore(const QString& userId) {
    const QString normalized = userId.trimmed();
    if (normalized.isEmpty() || normalized == m_userId) {
        return nullptr;
    }

    auto it = m_remoteVideoFrameStoresByPeer.find(normalized);
    if (it != m_remoteVideoFrameStoresByPeer.end()) {
        return it.value();
    }

    auto* store = new av::render::VideoFrameStore(this);
    m_remoteVideoFrameStoresByPeer.insert(normalized, store);
    emit remoteVideoFrameSourceChanged();
    return store;
}

void MeetingController::clearRemoteVideoFrameStores() {
    for (auto it = m_remoteVideoFrameStoresByPeer.begin(); it != m_remoteVideoFrameStoresByPeer.end(); ++it) {
        if (it.value() != nullptr) {
            it.value()->deleteLater();
        }
    }
    m_remoteVideoFrameStoresByPeer.clear();
    emit remoteVideoFrameSourceChanged();
}

void MeetingController::resetVideoPeerState(bool clearRemoteFrame) {
    if (!m_videoTransportKey.isEmpty()) {
        m_videoOfferSentPeers.remove(m_videoTransportKey);
        m_videoAnswerSentPeers.remove(m_videoTransportKey);
    }
    m_videoNegotiationStarted = false;
    m_videoPeerUserId.clear();
    m_videoTransportKey.clear();
    if (m_screenShareSession) {
        m_screenShareSession->setExpectedRemoteVideoSsrc(0U);
    }
    updateActiveVideoPeerSelection();

    if (clearRemoteFrame) {
        invalidateRemoteVideoRenderQueue(true);
        if (m_remoteScreenFrameStore) {
            m_remoteScreenFrameStore->clear();
        }
    }
    resetRemoteVideoAvSyncStats();

    if (m_mediaSessionManager) {
        m_mediaSessionManager->reset();
    }
}

void MeetingController::syncLocalParticipantMediaState() {
    if (m_userId.isEmpty()) {
        return;
    }

    ParticipantListModel::ParticipantItem existingItem;
    bool hasExistingItem = false;
    for (const auto& item : m_participantModel->items()) {
        if (item.userId == m_userId) {
            existingItem = item;
            hasExistingItem = true;
            break;
        }
    }

    if (!hasExistingItem && !m_inMeeting) {
        return;
    }

    const QString displayName = hasExistingItem
                                    ? existingItem.displayName
                                    : (m_username.isEmpty() ? QStringLiteral("You") : m_username);
    const QString avatarUrl = hasExistingItem ? existingItem.avatarUrl : QString();
    const int role = hasExistingItem ? existingItem.role : (m_currentMeetingHost ? 1 : 0);

    m_participantModel->upsertParticipant(m_userId,
                                          displayName,
                                          avatarUrl,
                                          role,
                                          !m_audioMuted,
                                          !m_videoMuted,
                                          m_screenSharing,
                                          m_assignedAudioSsrc,
                                          m_assignedVideoSsrc);
    if (m_currentMeetingHost || role == 1) {
        m_participantModel->setHostUserId(m_userId);
    }
}

void MeetingController::setScreenSharing(bool sharing) {
    if (m_screenSharing == sharing) {
        return;
    }

    m_screenSharing = sharing;
    emit screenSharingChanged();
    if (m_screenSharing) {
        if (m_localVideoFrameStore) {
            m_localVideoFrameStore->clear();
        }
        invalidateRemoteVideoRenderQueue(true);
    }
    updateActiveVideoPeerSelection();
}

void MeetingController::resetMediaNegotiation() {
    resetAudioPeerState();
    resetVideoPeerState(true);
    m_audioOfferSentPeers.clear();
    m_audioAnswerSentPeers.clear();
    m_videoOfferSentPeers.clear();
    m_videoAnswerSentPeers.clear();
    m_remoteVideoSsrcByPeer.clear();
    m_assignedAudioSsrc = 0;
    m_assignedVideoSsrc = 0;
    m_audioClientIceUfrag.clear();
    m_videoClientIceUfrag.clear();

    if (m_audioCallSession) {
        m_audioCallSession->stop();
        m_audioCallSession.reset();
    }

    if (m_screenShareSession) {
        m_screenShareSession->stop();
        m_screenShareSession.reset();
    }

}

void MeetingController::maybeStartMediaNegotiation() {
    maybeStartAudioNegotiation();
    maybeStartVideoNegotiation();
}

void MeetingController::maybeStartAudioNegotiation() {
    if (!m_loggedIn || !m_inMeeting || m_meetingId.isEmpty()) {
        return;
    }

    const QString transportKey = currentAudioTransportKey();
    const bool useSfuTransport = transportKey == QLatin1String(kSfuTransportKey);
    const QString peerUserId = currentAudioPeerUserId();
    if (peerUserId.isEmpty() && !useSfuTransport) {
        resetAudioPeerState();
        if (m_audioCallSession) {
            m_audioCallSession->stop();
            m_audioCallSession.reset();
        }
        return;
    }

    if (transportKey.isEmpty()) {
        resetAudioPeerState();
        return;
    }

    if (!m_audioTransportKey.isEmpty() && m_audioTransportKey != transportKey) {
        resetAudioPeerState();
    }

    if (!m_audioSessionManager) {
        m_audioSessionManager = std::make_unique<MediaSessionManager>();
    }

    if (!m_audioCallSession) {
        av::session::AudioCallSessionConfig config{};
        config.localAddress = "0.0.0.0";
        config.localPort = 0;
        config.peerAddress = "127.0.0.1";
        config.peerPort = 0;
        config.sampleRate = kAudioSampleRate;
        config.channels = kAudioChannels;
        config.frameSamples = kAudioFrameSamples;
        config.bitrate = kAudioBitrate;
        m_audioCallSession = std::make_unique<av::session::AudioCallSession>(config);
        if (m_assignedAudioSsrc != 0U) {
            m_audioCallSession->setAudioSsrc(m_assignedAudioSsrc);
        }
    }

    m_audioCallSession->setCaptureMuted(m_audioMuted);
    (void)m_audioCallSession->setPreferredInputDeviceName(m_preferredMicrophoneDevice);
    (void)m_audioCallSession->setPreferredOutputDeviceName(m_preferredSpeakerDevice);

    if (!m_audioCallSession->isRunning()) {
        if (!m_audioCallSession->start()) {
            emit infoMessage(QStringLiteral("Failed to start audio session: %1")
                                 .arg(QString::fromStdString(m_audioCallSession->lastError())));
            return;
        }
        m_audioNegotiationStarted = true;
    }

    if (!peerUserId.isEmpty()) {
        m_audioPeerUserId = peerUserId;
    }
    m_audioTransportKey = transportKey;
    updateAudioSessionSettings();

    if (!m_signaling->isConnected()) {
        return;
    }

    sendAudioOfferToPeer(false);
}

void MeetingController::maybeStartVideoNegotiation() {
    if (!m_loggedIn || !m_inMeeting || m_meetingId.isEmpty()) {
        return;
    }

    const QString transportKey = currentVideoTransportKey();
    const bool useSfuTransport = transportKey == QLatin1String(kSfuTransportKey);
    const QString peerUserId = currentVideoPeerUserId();
    if (peerUserId.isEmpty() && !useSfuTransport) {
        resetVideoPeerState(true);
        if (m_screenShareSession) {
            m_screenShareSession->stop();
            m_screenShareSession.reset();
        }
        return;
    }

    if (transportKey.isEmpty()) {
        resetVideoPeerState(true);
        return;
    }

    if (!m_videoTransportKey.isEmpty() && m_videoTransportKey != transportKey) {
        resetVideoPeerState(false);
    }

    if (!m_mediaSessionManager) {
        m_mediaSessionManager = std::make_unique<MediaSessionManager>();
    }

    if (!m_screenShareSession) {
        const auto& videoTuning = videoSessionTuning();
        av::session::ScreenShareSessionConfig config{};
        config.localAddress = "0.0.0.0";
        config.localPort = 0;
        config.peerAddress = "127.0.0.1";
        config.peerPort = 0;
        config.width = videoTuning.width;
        config.height = videoTuning.height;
        config.frameRate = videoTuning.frameRate;
        config.bitrate = videoTuning.bitrate;
        config.encoderPreset = videoTuning.encoderPreset;
        config.cameraPayloadType = static_cast<uint8_t>(kCameraPayloadType);
        config.payloadType = static_cast<uint8_t>(kScreenPayloadType);
        m_screenShareSession = std::make_unique<av::session::ScreenShareSession>(config);
        if (m_assignedVideoSsrc != 0U) {
            m_screenShareSession->setVideoSsrc(m_assignedVideoSsrc);
        }
        emit infoMessage(QStringLiteral("Video profile active: %1x%2@%3fps bitrate=%4 preset=%5 encoder=%6")
                             .arg(videoTuning.width)
                             .arg(videoTuning.height)
                             .arg(videoTuning.frameRate)
                             .arg(videoTuning.bitrate)
                             .arg(videoTuning.profileName)
                             .arg(videoEncoderPresetLabel(videoTuning.encoderPreset)));
        m_screenShareSession->setPreferredCameraDeviceName(m_preferredCameraDevice.toStdString());
        m_screenShareSession->setDecodedFrameWithSsrcCallback(
            [this](av::codec::DecodedVideoFrame frame, uint32_t remoteMediaSsrc) {
            if (!m_remoteScreenFrameStore) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, frame = std::move(frame), remoteMediaSsrc]() mutable {
                ++m_remoteVideoDecodedFrameCount;
                int renderDelayMs = 0;
                if (m_audioCallSession) {
                    const auto clock = m_audioCallSession->clock();
                    renderDelayMs = suggestRemoteVideoRenderDelayMsByAudioClock(frame, clock);
                }

                bool hasActiveScreenShare = false;
                for (const auto& participant : m_participantModel->items()) {
                    if (participant.userId == m_activeShareUserId && participant.userId != m_userId && participant.sharing) {
                        hasActiveScreenShare = true;
                        break;
                    }
                }

                if (hasActiveScreenShare) {
                    invalidateRemoteVideoRenderQueue(true);
                    if (m_remoteScreenFrameStore) {
                        m_remoteScreenFrameStore->setFrame(std::move(frame));
                    }
                    return;
                }

                if (m_remoteScreenFrameStore) {
                    m_remoteScreenFrameStore->clear();
                }

                const QString framePeerUserId = resolvePeerUserIdForRemoteVideoSsrc(remoteMediaSsrc);
                auto framePtr = std::make_shared<av::codec::DecodedVideoFrame>(std::move(frame));
                const int64_t framePts = framePtr->pts;
                if (!framePeerUserId.isEmpty()) {
                    (void)ensureRemoteVideoFrameStore(framePeerUserId);
                }
                enqueueRemoteVideoRenderTask([this, framePeerUserId, framePtr = std::move(framePtr)]() {
                    if (hasActiveShare()) {
                        return;
                    }
                    const QString activePeerUserId = m_activeVideoPeerUserId.trimmed();
                    const bool shouldUpdateDefaultStore = framePeerUserId.isEmpty() ||
                                                          activePeerUserId.isEmpty() ||
                                                          activePeerUserId == framePeerUserId;
                    if (shouldUpdateDefaultStore && m_remoteVideoFrameStore) {
                        m_remoteVideoFrameStore->setFrame(framePtr);
                    }
                    if (!framePeerUserId.isEmpty()) {
                        if (auto* peerStore = ensureRemoteVideoFrameStore(framePeerUserId)) {
                            peerStore->setFrame(framePtr);
                        }
                    }
                }, renderDelayMs, framePts, framePeerUserId);
            },
                Qt::QueuedConnection);
        });
        m_screenShareSession->setCameraSourceCallback([this](bool syntheticFallback) {
            QMetaObject::invokeMethod(this, [this, syntheticFallback]() {
                emit infoMessage(syntheticFallback
                                     ? QStringLiteral("Video camera source: synthetic-fallback")
                                     : QStringLiteral("Video camera source: real-device"));
            }, Qt::QueuedConnection);
        });
        m_screenShareSession->setLocalCameraPreviewCallback([this](av::codec::DecodedVideoFrame frame) {
            QMetaObject::invokeMethod(
                this,
                [this, frame = std::move(frame)]() mutable {
                    if (!m_localVideoFrameStore || !m_inMeeting || m_videoMuted || m_screenSharing) {
                        return;
                    }
                    m_localVideoFrameStore->setFrame(std::move(frame));
                },
                Qt::QueuedConnection);
        });
        m_screenShareSession->setStatusCallback([this](std::string statusMessage) {
            const QString statusText = QString::fromStdString(statusMessage);
            QMetaObject::invokeMethod(this, [this, statusText]() {
                emit infoMessage(statusText);
            }, Qt::QueuedConnection);
        });
        m_screenShareSession->setErrorCallback([this](std::string errorMessage) {
            const QString errorText = QString::fromStdString(errorMessage);
            QMetaObject::invokeMethod(this, [this, errorText]() {
                const bool encoderUnavailable =
                    errorText.contains(QStringLiteral("video encoder configure failed"), Qt::CaseInsensitive);
                if (encoderUnavailable) {
                    emit infoMessage(QStringLiteral("Video encoder unavailable, attempting camera auto-mute downgrade"));
                } else {
                    emit infoMessage(QStringLiteral("Video session error: %1").arg(errorText));
                }
                if (!m_screenShareSession || !m_inMeeting || m_screenSharing || m_videoMuted) {
                    return;
                }
                if (m_screenShareSession->cameraSendingEnabled()) {
                    return;
                }

                m_videoMuted = true;
                emit videoMutedChanged();
                syncLocalParticipantMediaState();
                updateVideoSessionSettings();
                if (m_signaling && m_signaling->isConnected()) {
                    m_signaling->sendMediaMuteToggle(1, true);
                    sendVideoOfferToPeer(true);
                }
            }, Qt::QueuedConnection);
        });
    }

    if (m_screenShareSession && !m_screenShareSession->isRunning()) {
        if (!m_screenShareSession->start()) {
            emit infoMessage(QStringLiteral("Failed to start screen session: %1")
                                 .arg(QString::fromStdString(m_screenShareSession->lastError())));
            m_screenShareSession.reset();
        }
    }

    if (m_screenShareSession && m_screenSharing) {
        if (!m_screenShareSession->setCameraSendingEnabled(false)) {
            emit infoMessage(QStringLiteral("Failed to stop camera sending: %1")
                                 .arg(QString::fromStdString(m_screenShareSession->lastError())));
        }
        if (!m_screenShareSession->setSharingEnabled(true)) {
            emit infoMessage(QStringLiteral("Failed to enable screen sharing: %1")
                                 .arg(QString::fromStdString(m_screenShareSession->lastError())));
        }
    }

    if (!peerUserId.isEmpty()) {
        m_videoPeerUserId = peerUserId;
    }
    m_videoTransportKey = transportKey;
    updateExpectedRemoteVideoSsrcForCurrentPeer();
    updateVideoSessionSettings();

    if (!m_signaling->isConnected()) {
        return;
    }

    sendVideoOfferToPeer(false);
}

bool MeetingController::sendAudioOfferToPeer(bool force) {
    if (!m_audioSessionManager || !m_signaling->isConnected()) {
        return false;
    }
    if (m_meetingId.trimmed().isEmpty()) {
        return false;
    }
    if (!resolveSfuEndpoint(nullptr, nullptr)) {
        emit infoMessage(QStringLiteral("Cannot send audio transport offer: SFU route is unavailable"));
        return false;
    }

    const QString transportKey = currentAudioTransportKey();
    if (transportKey.isEmpty()) {
        return false;
    }

    const bool useSfuTransport = transportKey == QLatin1String(kSfuTransportKey);
    if (m_audioOfferSentPeers.contains(transportKey) && (!force || useSfuTransport)) {
        return false;
    }

    updateAudioSessionSettings();
    const quint16 localPort = m_audioCallSession ? m_audioCallSession->localPort() : 0;
    const QStringList candidates = collectIceCandidates(localPort, m_iceServers, icePolicyFromEnvironment());
    if (candidates.isEmpty()) {
        emit infoMessage(QStringLiteral("Failed to build audio ICE candidates"));
        return false;
    }

    const QString clientIceUfrag = makeIceToken(8);
    const QString clientIcePwd = makeIceToken(24);
    const QString clientDtlsFingerprint = m_audioCallSession ? m_audioCallSession->prepareDtlsFingerprint() : QString{};
    if (clientDtlsFingerprint.isEmpty()) {
        emit infoMessage(QStringLiteral("Failed to prepare audio DTLS fingerprint"));
        return false;
    }
    m_audioClientIceUfrag = clientIceUfrag;
    const bool publishAudio = !m_audioMuted;
    m_signaling->sendTransportOffer(m_meetingId,
                                    publishAudio,
                                    false,
                                    clientIceUfrag,
                                    clientIcePwd,
                                    clientDtlsFingerprint,
                                    candidates);
    m_audioOfferSentPeers.insert(transportKey);
    m_audioAnswerSentPeers.remove(transportKey);
    if (shouldEmitVerboseNegotiationLogs()) {
        const QString offerTarget = transportKey == QLatin1String(kSfuTransportKey)
                                        ? QStringLiteral("sfu")
                                        : currentAudioPeerUserId();
        emit infoMessage(QStringLiteral("Audio transport offer sent to SFU for %1").arg(offerTarget));
    }
    return true;
}

bool MeetingController::sendVideoOfferToPeer(bool force) {
    if (!m_mediaSessionManager || !m_signaling->isConnected()) {
        return false;
    }
    if (m_meetingId.trimmed().isEmpty()) {
        return false;
    }
    if (!resolveSfuEndpoint(nullptr, nullptr)) {
        emit infoMessage(QStringLiteral("Cannot send video transport offer: SFU route is unavailable"));
        return false;
    }

    const QString transportKey = currentVideoTransportKey();
    if (transportKey.isEmpty()) {
        return false;
    }

    const bool useSfuTransport = transportKey == QLatin1String(kSfuTransportKey);
    if (m_videoOfferSentPeers.contains(transportKey) && (!force || useSfuTransport)) {
        return false;
    }

    updateVideoSessionSettings();
    const quint32 videoSsrc = currentVideoSsrc();
    if (!useSfuTransport && videoSsrc == 0U) {
        return false;
    }
    const quint16 localPort = m_screenShareSession ? m_screenShareSession->localPort() : 0;
    const QStringList candidates = collectIceCandidates(localPort, m_iceServers, icePolicyFromEnvironment());
    if (candidates.isEmpty()) {
        emit infoMessage(QStringLiteral("Failed to build video ICE candidates"));
        return false;
    }

    const QString clientIceUfrag = makeIceToken(8);
    const QString clientIcePwd = makeIceToken(24);
    const QString clientDtlsFingerprint = m_screenShareSession ? m_screenShareSession->prepareDtlsFingerprint() : QString{};
    if (clientDtlsFingerprint.isEmpty()) {
        emit infoMessage(QStringLiteral("Failed to prepare video DTLS fingerprint"));
        return false;
    }
    m_videoClientIceUfrag = clientIceUfrag;
    const bool publishVideo = useSfuTransport || m_screenSharing || !m_videoMuted;
    m_signaling->sendTransportOffer(m_meetingId,
                                    false,
                                    publishVideo,
                                    clientIceUfrag,
                                    clientIcePwd,
                                    clientDtlsFingerprint,
                                    candidates);
    m_videoOfferSentPeers.insert(transportKey);
    m_videoAnswerSentPeers.remove(transportKey);
    if (shouldEmitVerboseNegotiationLogs()) {
        const QString offerTarget = transportKey == QLatin1String(kSfuTransportKey)
                                        ? QStringLiteral("sfu")
                                        : currentVideoPeerUserId();
        emit infoMessage(QStringLiteral("Video transport offer sent to SFU for %1").arg(offerTarget));
    }
    return true;
}


































