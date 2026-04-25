#include "server/MediaIngress.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include "room/Publisher.h"
#include "room/Room.h"
#include "room/Subscriber.h"

namespace sfu {
namespace {

std::string StripUdpScheme(std::string endpointText) {
    if (endpointText.rfind("udp://", 0) == 0) {
        endpointText.erase(0, 6);
    }
    return endpointText;
}

std::vector<uint8_t> BuildRembPacket(uint32_t senderSsrc,
                                     uint32_t mediaSsrc,
                                     uint8_t bitrateExp,
                                     uint32_t bitrateMantissa,
                                     uint32_t targetSsrc) {
    const uint32_t cappedMantissa = bitrateMantissa & 0x3FFFFU;
    return std::vector<uint8_t>{
        0x8F, 0xCE, 0x00, 0x05,
        static_cast<uint8_t>((senderSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((senderSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((senderSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(senderSsrc & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((mediaSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(mediaSsrc & 0xFF),
        'R', 'E', 'M', 'B',
        0x01,
        static_cast<uint8_t>((static_cast<uint8_t>(bitrateExp & 0x3FU) << 2U) | static_cast<uint8_t>((cappedMantissa >> 16U) & 0x03U)),
        static_cast<uint8_t>((cappedMantissa >> 8U) & 0xFFU),
        static_cast<uint8_t>(cappedMantissa & 0xFFU),
        static_cast<uint8_t>((targetSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((targetSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((targetSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(targetSsrc & 0xFF),
    };
}

std::pair<uint8_t, uint32_t> EncodeRembBitrate(uint32_t bitrateBps) {
    uint8_t exp = 0;
    uint32_t mantissa = bitrateBps;
    while (mantissa > 0x3FFFFU && exp < 63U) {
        mantissa = (mantissa + 1U) >> 1U;
        ++exp;
    }
    return {exp, static_cast<uint32_t>(mantissa & 0x3FFFFU)};
}

std::vector<uint8_t> BuildReceiverReportPacket(uint32_t receiverSsrc,
                                               uint32_t sourceSsrc,
                                               uint32_t lastSenderReport,
                                               uint32_t delaySinceLastSenderReport) {
    return std::vector<uint8_t>{
        0x81, 0xC9, 0x00, 0x07,
        static_cast<uint8_t>((receiverSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((receiverSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((receiverSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(receiverSsrc & 0xFF),
        static_cast<uint8_t>((sourceSsrc >> 24) & 0xFF),
        static_cast<uint8_t>((sourceSsrc >> 16) & 0xFF),
        static_cast<uint8_t>((sourceSsrc >> 8) & 0xFF),
        static_cast<uint8_t>(sourceSsrc & 0xFF),
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        static_cast<uint8_t>((lastSenderReport >> 24) & 0xFF),
        static_cast<uint8_t>((lastSenderReport >> 16) & 0xFF),
        static_cast<uint8_t>((lastSenderReport >> 8) & 0xFF),
        static_cast<uint8_t>(lastSenderReport & 0xFF),
        static_cast<uint8_t>((delaySinceLastSenderReport >> 24) & 0xFF),
        static_cast<uint8_t>((delaySinceLastSenderReport >> 16) & 0xFF),
        static_cast<uint8_t>((delaySinceLastSenderReport >> 8) & 0xFF),
        static_cast<uint8_t>(delaySinceLastSenderReport & 0xFF),
    };
}

} // namespace

MediaIngress::MediaIngress(std::shared_ptr<RoomManager> roomManager, uint16_t listenPort, std::size_t nackCapacity)
    : udpServer_(2048, listenPort)
    , roomManager_(roomManager ? std::move(roomManager) : std::make_shared<RoomManager>())
    , router_(nackCapacity) {
    running_.store(udpServer_.Start([this](const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from) {
        (void)HandlePacket(data, len, from);
    }));
}

MediaIngress::~MediaIngress() {
    running_.store(false);
    udpServer_.Stop();
}

uint16_t MediaIngress::Port() const noexcept {
    return udpServer_.Port();
}

bool MediaIngress::IsRunning() const noexcept {
    return running_.load();
}

bool MediaIngress::BindPublisher(const std::string& meetingId,
                                 const std::string& userId,
                                 uint32_t audioSsrc,
                                 uint32_t videoSsrc) {
    if (meetingId.empty() || userId.empty() || !roomManager_) {
        return false;
    }

    const auto room = roomManager_->GetRoomShared(meetingId);
    if (!room) {
        return false;
    }

    const auto existing = room->GetPublisher(userId);
    const uint32_t previousAudioSsrc = existing ? existing->AudioSsrc() : 0U;
    const uint32_t previousVideoSsrc = existing ? existing->VideoSsrc() : 0U;
    const uint32_t mergedAudioSsrc = audioSsrc != 0 ? audioSsrc : previousAudioSsrc;
    const uint32_t mergedVideoSsrc = videoSsrc != 0 ? videoSsrc : previousVideoSsrc;
    if (mergedAudioSsrc == 0 && mergedVideoSsrc == 0) {
        return false;
    }

    const auto publisher = std::make_shared<Publisher>(userId, mergedAudioSsrc, mergedVideoSsrc);
    if (!room->AddPublisher(publisher)) {
        return false;
    }

    if (previousAudioSsrc != 0 && previousAudioSsrc != mergedAudioSsrc) {
        RemovePublisherRoutes(previousAudioSsrc);
    }
    if (previousVideoSsrc != 0 && previousVideoSsrc != mergedVideoSsrc) {
        RemovePublisherRoutes(previousVideoSsrc);
    }

    if (mergedAudioSsrc != 0) {
        router_.RegisterPublisher(mergedAudioSsrc);
    }
    if (mergedVideoSsrc != 0) {
        router_.RegisterPublisher(mergedVideoSsrc);
    }

    RefreshRoomRoutes(meetingId);
    return true;
}

bool MediaIngress::BindSubscriber(const std::string& meetingId,
                                  const std::string& userId,
                                  uint32_t audioSsrc,
                                  uint32_t videoSsrc) {
    if (meetingId.empty() || userId.empty() || !roomManager_) {
        return false;
    }

    const auto room = roomManager_->GetRoomShared(meetingId);
    if (!room) {
        return false;
    }

    const auto existing = room->GetSubscriber(userId);
    const uint32_t mergedAudioSsrc = audioSsrc != 0
        ? audioSsrc
        : (existing ? existing->AudioSsrc() : 0U);
    const uint32_t mergedVideoSsrc = videoSsrc != 0
        ? videoSsrc
        : (existing ? existing->VideoSsrc() : 0U);
    const std::string fallbackEndpoint = existing ? existing->Endpoint() : std::string{};
    auto subscriber = std::make_shared<Subscriber>(userId, fallbackEndpoint, mergedAudioSsrc, mergedVideoSsrc);
    if (existing) {
        subscriber->SetAudioEndpoint(existing->AudioEndpoint());
        subscriber->SetVideoEndpoint(existing->VideoEndpoint());
    }
    if (!room->AddSubscriber(subscriber)) {
        return false;
    }

    RefreshRoomRoutes(meetingId);
    return true;
}

bool MediaIngress::SetupTransport(const std::string& meetingId,
                                  const std::string& userId,
                                  bool publishAudio,
                                  bool publishVideo,
                                  const std::string& clientIceUfrag,
                                  const std::string& clientIcePwd,
                                  const std::string& clientDtlsFingerprint,
                                  const std::vector<std::string>& clientCandidates,
                                  const std::string& advertisedAddress,
                                  TransportSetupResult* result) {
    if (result == nullptr) {
        return false;
    }
    *result = {};
    if (meetingId.empty() || userId.empty() ||
        clientIceUfrag.empty() || clientIcePwd.empty() || clientDtlsFingerprint.empty() ||
        !roomManager_) {
        return false;
    }

    const auto room = roomManager_->GetRoomShared(meetingId);
    if (!room || !dtlsContext_.Initialize()) {
        return false;
    }

    const uint32_t assignedAudioSsrc = publishAudio ? MakeRandomSsrc() : 0U;
    const uint32_t assignedVideoSsrc = publishVideo ? MakeRandomSsrc() : 0U;
    const std::string serverIceUfrag = MakeRandomIceToken(8);
    const std::string serverIcePwd = MakeRandomIceToken(24);
    const std::string host = HostFromAdvertisedAddress(advertisedAddress);
    std::vector<std::string> serverCandidates;
    serverCandidates.push_back(MakeHostCandidate(host.empty() ? "127.0.0.1" : host, Port()));

    IceLiteSession iceSession;
    iceSession.Configure(clientIceUfrag,
                         clientIcePwd,
                         clientCandidates,
                         serverIceUfrag,
                         serverIcePwd,
                         serverCandidates);
    auto dtlsTransport = std::make_unique<DtlsTransport>(dtlsContext_, DtlsTransport::Role::Server);
    if (!dtlsTransport->Start(clientDtlsFingerprint)) {
        return false;
    }
    transportRegistry_.Upsert(ParticipantTransport(meetingId,
                                                   userId,
                                                   publishAudio,
                                                   publishVideo,
                                                   clientDtlsFingerprint,
                                                   assignedAudioSsrc,
                                                   assignedVideoSsrc,
                                                   std::move(iceSession),
                                                   std::move(dtlsTransport)),
                              TransportRegistry::Kind(publishAudio, publishVideo));

    if (!BindSubscriber(meetingId, userId, assignedAudioSsrc, assignedVideoSsrc)) {
        return false;
    }
    if ((assignedAudioSsrc != 0 || assignedVideoSsrc != 0) &&
        !BindPublisher(meetingId, userId, assignedAudioSsrc, assignedVideoSsrc)) {
        return false;
    }

    result->success = true;
    result->serverIceUfrag = std::move(serverIceUfrag);
    result->serverIcePwd = std::move(serverIcePwd);
    result->serverDtlsFingerprint = dtlsContext_.FingerprintSha256();
    result->serverCandidates = std::move(serverCandidates);
    result->assignedAudioSsrc = assignedAudioSsrc;
    result->assignedVideoSsrc = assignedVideoSsrc;
    return true;
}

bool MediaIngress::TrickleIceCandidate(const std::string& meetingId,
                                       const std::string& userId,
                                       const std::string& candidate,
                                       const std::string& sdpMid,
                                       bool endOfCandidates) {
    (void)sdpMid;
    if (meetingId.empty() || userId.empty()) {
        return false;
    }

    return transportRegistry_.AppendIceCandidate(meetingId, userId, candidate, endOfCandidates);
}

bool MediaIngress::CloseTransport(const std::string& meetingId, const std::string& userId) {
    if (meetingId.empty() || userId.empty()) {
        return false;
    }
    (void)transportRegistry_.EraseUserTransports(meetingId, userId);
    (void)RemoveSubscriber(meetingId, userId);
    (void)RemovePublisher(meetingId, userId);
    return true;
}

bool MediaIngress::RemovePublisher(const std::string& meetingId, const std::string& userId) {
    if (meetingId.empty() || userId.empty() || !roomManager_) {
        return false;
    }

    const auto room = roomManager_->GetRoomShared(meetingId);
    if (!room) {
        return false;
    }

    const auto publisher = room->GetPublisher(userId);
    const bool removed = room->RemovePublisher(userId);
    if (!removed) {
        return false;
    }

    if (publisher) {
        RemovePublisherRoutes(publisher->AudioSsrc());
        RemovePublisherRoutes(publisher->VideoSsrc());
    }

    {
        std::lock_guard<std::mutex> lock(publisherTrafficMutex_);
        publisherTraffic_.erase(PublisherTrafficKey(meetingId, userId));
        if (publisher) {
            if (publisher->AudioSsrc() != 0) {
                senderReports_.erase(publisher->AudioSsrc());
            }
            if (publisher->VideoSsrc() != 0) {
                senderReports_.erase(publisher->VideoSsrc());
            }
        }
    }

    return true;
}

bool MediaIngress::RemoveSubscriber(const std::string& meetingId, const std::string& userId) {
    if (meetingId.empty() || userId.empty() || !roomManager_) {
        return false;
    }

    const auto room = roomManager_->GetRoomShared(meetingId);
    if (!room) {
        return false;
    }

    const bool removed = room->RemoveSubscriber(userId);
    if (!removed) {
        return false;
    }

    RefreshRoomRoutes(meetingId);
    return true;
}

bool MediaIngress::ResolvePublisher(uint32_t ssrc, RoomManager::PublisherLocation* out) const {
    return roomManager_ && roomManager_->FindPublisherBySsrc(ssrc, out);
}

std::size_t MediaIngress::PacketCount() const noexcept {
    return packetCount_.load();
}

bool MediaIngress::LastObservation(PacketObservation* out) const {
    if (out == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(observationMutex_);
    *out = lastObservation_;
    return true;
}

std::vector<MediaIngress::PublisherTrafficSnapshot> MediaIngress::SnapshotPublisherTraffic() const {
    std::vector<PublisherTrafficSnapshot> snapshots;
    std::lock_guard<std::mutex> lock(publisherTrafficMutex_);
    for (auto it = publisherTraffic_.begin(); it != publisherTraffic_.end();) {
        const auto room = roomManager_ ? roomManager_->GetRoomShared(it->second.meetingId) : nullptr;
        if (!room || room->GetPublisher(it->second.userId) == nullptr) {
            it = publisherTraffic_.erase(it);
            continue;
        }

        snapshots.push_back(PublisherTrafficSnapshot{
            it->second.meetingId,
            it->second.userId,
            it->second.packetCount,
            it->second.byteCount,
            it->second.packetLoss,
            it->second.jitterMs,
            it->second.rttMs,
        });
        ++it;
    }

    std::sort(snapshots.begin(), snapshots.end(), [](const PublisherTrafficSnapshot& left, const PublisherTrafficSnapshot& right) {
        if (left.meetingId != right.meetingId) {
            return left.meetingId < right.meetingId;
        }
        return left.userId < right.userId;
    });
    return snapshots;
}

bool MediaIngress::SendEstimatedRembToPublisher(const std::string& meetingId,
                                                const std::string& userId,
                                                uint32_t bitrateKbps) {
    if (meetingId.empty() || userId.empty() || bitrateKbps == 0 || !roomManager_) {
        return false;
    }

    const auto room = roomManager_->GetRoomShared(meetingId);
    if (!room) {
        return false;
    }
    const auto publisher = room->GetPublisher(userId);
    if (!publisher) {
        return false;
    }

    const uint32_t sourceSsrc = publisher->VideoSsrc() != 0 ? publisher->VideoSsrc() : publisher->AudioSsrc();
    if (sourceSsrc == 0) {
        return false;
    }

    UdpServer::Endpoint publisherEndpoint{};
    if (!ResolvePublisherEndpoint(sourceSsrc, &publisherEndpoint)) {
        return false;
    }

    constexpr uint32_t kMaxKbpsBeforeOverflow = 4294967U;
    const uint32_t cappedKbps = std::min<uint32_t>(bitrateKbps, kMaxKbpsBeforeOverflow);
    const uint32_t bitrateBps = cappedKbps * 1000U;
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kMinSendInterval = std::chrono::milliseconds(500);
    constexpr uint32_t kMinChangePercent = 5U;

    bool shouldSend = true;
    {
        std::lock_guard<std::mutex> lock(estimatedRembMutex_);
        auto& state = estimatedRembBySource_[sourceSsrc];
        if (state.sentAt.time_since_epoch().count() != 0) {
            const auto elapsed = now - state.sentAt;
            const uint32_t oldBitrateBps = state.bitrateBps;
            const uint32_t deltaBps = oldBitrateBps > bitrateBps
                ? oldBitrateBps - bitrateBps
                : bitrateBps - oldBitrateBps;
            const uint32_t changePercent = oldBitrateBps == 0
                ? 100U
                : static_cast<uint32_t>((static_cast<uint64_t>(deltaBps) * 100ULL) / oldBitrateBps);
            if (elapsed < kMinSendInterval && changePercent < kMinChangePercent) {
                shouldSend = false;
            }
        }
        if (shouldSend) {
            state.bitrateBps = bitrateBps;
            state.sentAt = now;
        }
    }
    if (!shouldSend) {
        return false;
    }

    const auto [exp, mantissa] = EncodeRembBitrate(bitrateBps);
    auto packet = BuildRembPacket(0U, sourceSsrc, exp, mantissa, sourceSsrc);
    if (packet.empty()) {
        return false;
    }
    if (EndpointRequiresSecureMedia(publisherEndpoint) &&
        !transportRegistry_.ProtectRtcpToEndpoint(publisherEndpoint, &packet)) {
        return false;
    }

    udpServer_.SendTo(packet.data(), packet.size(), publisherEndpoint);
    return true;
}

bool MediaIngress::ParseEndpoint(const std::string& endpointText, UdpServer::Endpoint* endpoint) {
    if (endpoint == nullptr || endpointText.empty()) {
        return false;
    }

    const auto normalized = StripUdpScheme(endpointText);
    const auto colon = normalized.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= normalized.size()) {
        return false;
    }

    const auto hostText = normalized.substr(0, colon);
    const auto portText = normalized.substr(colon + 1);

    in_addr address{};
#ifdef _WIN32
    const auto ok = InetPtonA(AF_INET, hostText.c_str(), &address);
#else
    const auto ok = inet_pton(AF_INET, hostText.c_str(), &address);
#endif
    if (ok != 1) {
        return false;
    }

    const auto port = static_cast<uint16_t>(std::strtoul(portText.c_str(), nullptr, 10));
    if (port == 0) {
        return false;
    }

    std::memset(endpoint, 0, sizeof(*endpoint));
    endpoint->sin_family = AF_INET;
    endpoint->sin_port = htons(port);
    endpoint->sin_addr = address;
    return true;
}

std::string MediaIngress::HostFromAdvertisedAddress(const std::string& advertisedAddress) {
    const auto normalized = StripUdpScheme(advertisedAddress);
    const auto colon = normalized.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        return {};
    }
    return normalized.substr(0, colon);
}

std::string MediaIngress::MakeHostCandidate(const std::string& host, uint16_t port) {
    return "candidate:1 1 udp 2130706431 " + host + " " + std::to_string(port) + " typ host";
}

std::string MediaIngress::MakeRandomIceToken(std::size_t bytes) {
    if (bytes == 0) {
        return {};
    }
    std::vector<unsigned char> random(bytes);
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        return {};
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : random) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

uint32_t MediaIngress::MakeRandomSsrc() {
    uint32_t value = 0;
    while (value == 0) {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), static_cast<int>(sizeof(value))) != 1) {
            return 0;
        }
        value = ntohl(value);
    }
    return value;
}

bool MediaIngress::IsStunPacket(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 20U) {
        return false;
    }
    if ((data[0] & 0xC0U) != 0U) {
        return false;
    }
    return data[4] == 0x21U && data[5] == 0x12U && data[6] == 0xA4U && data[7] == 0x42U;
}

std::string MediaIngress::ParseStunUsername(const uint8_t* data, std::size_t len) {
    if (!IsStunPacket(data, len)) {
        return {};
    }
    const std::size_t messageLength = (static_cast<std::size_t>(data[2]) << 8U) |
                                      static_cast<std::size_t>(data[3]);
    if (20U + messageLength > len) {
        return {};
    }

    std::size_t offset = 20U;
    const std::size_t end = 20U + messageLength;
    while (offset + 4U <= end) {
        const uint16_t attrType = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8U) |
                                                        static_cast<uint16_t>(data[offset + 1U]));
        const std::size_t attrLength = (static_cast<std::size_t>(data[offset + 2U]) << 8U) |
                                       static_cast<std::size_t>(data[offset + 3U]);
        offset += 4U;
        if (offset + attrLength > end) {
            return {};
        }
        if (attrType == 0x0006U) {
            return std::string(reinterpret_cast<const char*>(data + offset), attrLength);
        }
        offset += ((attrLength + 3U) & ~std::size_t{3U});
    }
    return {};
}

std::vector<uint8_t> MediaIngress::BuildStunBindingResponse(const uint8_t* request,
                                                            std::size_t len,
                                                            const UdpServer::Endpoint& from) {
    if (!IsStunPacket(request, len)) {
        return {};
    }

    std::vector<uint8_t> response(20U + 12U, 0U);
    response[0] = 0x01U;
    response[1] = 0x01U;
    response[2] = 0x00U;
    response[3] = 0x0CU;
    response[4] = 0x21U;
    response[5] = 0x12U;
    response[6] = 0xA4U;
    response[7] = 0x42U;
    std::memcpy(response.data() + 8U, request + 8U, 12U);

    response[20] = 0x00U;
    response[21] = 0x20U;
    response[22] = 0x00U;
    response[23] = 0x08U;
    response[24] = 0x00U;
    response[25] = 0x01U;

    const uint16_t xport = static_cast<uint16_t>(ntohs(from.sin_port) ^ 0x2112U);
    response[26] = static_cast<uint8_t>((xport >> 8U) & 0xFFU);
    response[27] = static_cast<uint8_t>(xport & 0xFFU);

    const uint32_t address = ntohl(from.sin_addr.s_addr) ^ 0x2112A442U;
    response[28] = static_cast<uint8_t>((address >> 24U) & 0xFFU);
    response[29] = static_cast<uint8_t>((address >> 16U) & 0xFFU);
    response[30] = static_cast<uint8_t>((address >> 8U) & 0xFFU);
    response[31] = static_cast<uint8_t>(address & 0xFFU);
    return response;
}

bool MediaIngress::LooksLikeRtcp(const uint8_t* data, std::size_t len) {
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

std::string MediaIngress::PublisherTrafficKey(const std::string& meetingId, const std::string& userId) {
    return meetingId + "\n" + userId;
}

std::string MediaIngress::EndpointKey(const UdpServer::Endpoint& endpoint) {
    char hostBuffer[INET_ADDRSTRLEN] = {0};
#ifdef _WIN32
    if (InetNtopA(AF_INET,
                  const_cast<in_addr*>(&(endpoint.sin_addr)),
                  hostBuffer,
                  static_cast<DWORD>(sizeof(hostBuffer))) == nullptr) {
        return {};
    }
#else
    if (inet_ntop(AF_INET, &(endpoint.sin_addr), hostBuffer, sizeof(hostBuffer)) == nullptr) {
        return {};
    }
#endif

    return std::string(hostBuffer) + ":" + std::to_string(ntohs(endpoint.sin_port));
}

std::string MediaIngress::FeedbackRouteKey(const UdpServer::Endpoint& endpoint, uint32_t mediaSsrc) {
    return EndpointKey(endpoint) + "\n" + std::to_string(mediaSsrc);
}

void MediaIngress::RewriteRtpSsrc(uint32_t ssrc, uint8_t* packet, std::size_t len) {
    if (packet == nullptr || len < RtpParser::kMinHeaderSize) {
        return;
    }

    packet[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
    packet[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    packet[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
    packet[11] = static_cast<uint8_t>(ssrc & 0xFF);
}

uint32_t MediaIngress::MakeRouteRewriteSsrc(const std::string& meetingId,
                                            const std::string& subscriberUserId,
                                            uint32_t sourceSsrc,
                                            bool video) {
    std::string key = meetingId;
    key.push_back('|');
    key += subscriberUserId;
    key.push_back('|');
    key += video ? "video" : "audio";
    key.push_back('|');
    key += std::to_string(sourceSsrc);

    uint32_t hash = 2166136261U;
    for (const unsigned char ch : key) {
        hash ^= static_cast<uint32_t>(ch);
        hash *= 16777619U;
    }

    hash &= 0x7FFFFFFFU;
    if (hash == 0U || hash == sourceSsrc) {
        hash ^= 0x5A5A5A5AU;
    }
    return hash == 0U ? 0x13572468U : hash;
}

uint32_t MediaIngress::JitterToMs(const RoomManager::PublisherLocation& location, uint32_t sourceSsrc, uint32_t jitter) {
    if (!location.publisher.publisher || sourceSsrc == 0 || jitter == 0) {
        return 0;
    }

    uint32_t clockRate = 0;
    if (location.publisher.publisher->AudioSsrc() == sourceSsrc) {
        clockRate = 48000U;
    } else if (location.publisher.publisher->VideoSsrc() == sourceSsrc) {
        clockRate = 90000U;
    }
    if (clockRate == 0) {
        return 0;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(jitter) * 1000ULL) / clockRate);
}

uint32_t MediaIngress::CompactNtpFromTimestamp(uint64_t ntpTimestamp) {
    return static_cast<uint32_t>((ntpTimestamp >> 16U) & 0xFFFFFFFFULL);
}

uint32_t MediaIngress::CompactNtpFromElapsed(std::chrono::steady_clock::duration elapsed) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (micros <= 0) {
        return 0;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(micros) * 65536ULL) / 1000000ULL);
}

uint32_t MediaIngress::CompactNtpToMs(uint32_t compactNtp) {
    if (compactNtp == 0) {
        return 0;
    }

    return static_cast<uint32_t>(((static_cast<uint64_t>(compactNtp) * 1000ULL) + 65535ULL) / 65536ULL);
}

uint32_t MediaIngress::ComputeRttMs(const SenderReportLedgerEntry& entry,
                                    uint32_t lastSenderReport,
                                    uint32_t delaySinceLastSenderReport,
                                    std::chrono::steady_clock::time_point now) {
    if (entry.compactNtp == 0 || lastSenderReport == 0 || entry.compactNtp != lastSenderReport || now <= entry.observedAt) {
        return 0;
    }

    const uint32_t arrivalCompactNtp = entry.compactNtp + CompactNtpFromElapsed(now - entry.observedAt);
    const uint64_t reflectedDelay = static_cast<uint64_t>(lastSenderReport) + static_cast<uint64_t>(delaySinceLastSenderReport);
    if (static_cast<uint64_t>(arrivalCompactNtp) <= reflectedDelay) {
        return 0;
    }

    return CompactNtpToMs(static_cast<uint32_t>(static_cast<uint64_t>(arrivalCompactNtp) - reflectedDelay));
}

bool MediaIngress::ShouldForwardPli(uint32_t sourceSsrc, std::chrono::steady_clock::time_point now) {
    if (sourceSsrc == 0) {
        return false;
    }

    constexpr auto kPliForwardCooldown = std::chrono::milliseconds(200);

    std::lock_guard<std::mutex> lock(pliForwardMutex_);
    auto it = pliForwardBySource_.find(sourceSsrc);
    if (it != pliForwardBySource_.end() && now - it->second < kPliForwardCooldown) {
        return false;
    }
    pliForwardBySource_[sourceSsrc] = now;
    return true;
}

bool MediaIngress::SelectAggregatedRembForSource(uint32_t sourceSsrc,
                                                 const UdpServer::Endpoint& from,
                                                 const RtcpRembFeedback& remb,
                                                 uint8_t* outBitrateExp,
                                                 uint32_t* outBitrateMantissa) {
    if (sourceSsrc == 0 || outBitrateExp == nullptr || outBitrateMantissa == nullptr) {
        return false;
    }

    const std::string endpointKey = EndpointKey(from);
    if (endpointKey.empty()) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kRembFeedbackTtl = std::chrono::seconds(3);

    std::lock_guard<std::mutex> lock(rembFeedbackMutex_);
    auto& perEndpoint = rembFeedbackBySource_[sourceSsrc];
    perEndpoint[endpointKey] = RembFeedbackSample{
        remb.bitrateExp,
        remb.bitrateMantissa,
        remb.bitrateBps,
        now,
    };

    for (auto it = perEndpoint.begin(); it != perEndpoint.end();) {
        if (now - it->second.observedAt > kRembFeedbackTtl) {
            it = perEndpoint.erase(it);
        } else {
            ++it;
        }
    }
    if (perEndpoint.empty()) {
        rembFeedbackBySource_.erase(sourceSsrc);
        return false;
    }

    auto selected = perEndpoint.begin();
    for (auto it = perEndpoint.begin(); it != perEndpoint.end(); ++it) {
        if (it->second.bitrateBps < selected->second.bitrateBps) {
            selected = it;
        }
    }

    *outBitrateExp = selected->second.bitrateExp;
    *outBitrateMantissa = selected->second.bitrateMantissa;
    return true;
}

bool MediaIngress::HandleStunPacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from) {
    const std::string username = ParseStunUsername(data, len);
    if (username.empty()) {
        return false;
    }

    std::string meetingId;
    std::string userId;
    std::string mediaKind;
    if (!transportRegistry_.SelectEndpointForUsername(username, from, &meetingId, &userId, &mediaKind)) {
        return false;
    }

    const std::string endpointText = EndpointKey(from);
    const auto room = roomManager_ ? roomManager_->GetRoomShared(meetingId) : nullptr;
    if (room && !endpointText.empty()) {
        if (auto subscriber = room->GetSubscriber(userId)) {
            if (mediaKind == "audio") {
                subscriber->SetAudioEndpoint(endpointText);
            } else if (mediaKind == "video") {
                subscriber->SetVideoEndpoint(endpointText);
            } else {
                subscriber->SetEndpoint(endpointText);
                subscriber->SetAudioEndpoint(endpointText);
                subscriber->SetVideoEndpoint(endpointText);
            }
            RefreshRoomRoutes(meetingId);
        }
    }

    const std::vector<uint8_t> response = BuildStunBindingResponse(data, len, from);
    if (!response.empty()) {
        udpServer_.SendTo(response.data(), response.size(), from);
    }
    return true;
}

bool MediaIngress::HandleDtlsPacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from) {
    std::vector<std::vector<uint8_t>> outgoingPackets;
    if (!transportRegistry_.HandleDtlsFromEndpoint(from, data, len, &outgoingPackets)) {
        return false;
    }
    for (const auto& packet : outgoingPackets) {
        if (!packet.empty()) {
            udpServer_.SendTo(packet.data(), packet.size(), from);
        }
    }
    return true;
}

bool MediaIngress::TransportAllowsRtp(uint32_t ssrc, const UdpServer::Endpoint& from) const {
    return transportRegistry_.TransportAllowsSsrcFrom(ssrc, from);
}

bool MediaIngress::TransportRequiresSrtp(uint32_t ssrc) const {
    return transportRegistry_.HasTransportForSsrc(ssrc);
}

bool MediaIngress::EndpointRequiresSecureMedia(const UdpServer::Endpoint& from) const {
    return transportRegistry_.HasSelectedTransportForEndpoint(from);
}

uint32_t MediaIngress::ResolveNackSourceSsrc(const UdpServer::Endpoint& from, uint32_t mediaSsrc) const {
    if (mediaSsrc == 0) {
        return 0;
    }

    RoomManager::PublisherLocation location;
    if (ResolvePublisher(mediaSsrc, &location)) {
        return mediaSsrc;
    }

    const std::lock_guard<std::mutex> lock(feedbackRouteMutex_);
    const auto it = feedbackRoutes_.find(FeedbackRouteKey(from, mediaSsrc));
    if (it == feedbackRoutes_.end()) {
        return 0;
    }
    return it->second;
}

bool MediaIngress::ResolvePublisherEndpoint(uint32_t sourceSsrc, UdpServer::Endpoint* out) const {
    if (sourceSsrc == 0 || out == nullptr) {
        return false;
    }

    const std::lock_guard<std::mutex> lock(publisherEndpointMutex_);
    const auto it = publisherEndpoints_.find(sourceSsrc);
    if (it == publisherEndpoints_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

void MediaIngress::RefreshRoomRoutes(const std::string& meetingId) {
    const auto room = roomManager_ ? roomManager_->GetRoomShared(meetingId) : nullptr;
    if (!room) {
        return;
    }

    const auto publishers = room->SnapshotPublishers();
    for (const auto& publisher : publishers) {
        if (!publisher) {
            continue;
        }

        if (publisher->AudioSsrc() != 0) {
            RefreshPublisherRoutes(meetingId, publisher->AudioSsrc());
        }
        if (publisher->VideoSsrc() != 0) {
            RefreshPublisherRoutes(meetingId, publisher->VideoSsrc());
        }
    }
}

void MediaIngress::RefreshPublisherRoutes(const std::string& meetingId, uint32_t sourceSsrc) {
    if (sourceSsrc == 0 || !roomManager_) {
        return;
    }

    const auto room = roomManager_->GetRoomShared(meetingId);
    if (!room) {
        return;
    }
    Room::PublisherLookup lookup;
    const bool resolvedPublisher = room->FindPublisherBySsrc(sourceSsrc, &lookup) && lookup.publisher;
    const bool sourceIsVideo = resolvedPublisher && lookup.publisher->VideoSsrc() == sourceSsrc;
    const std::string sourceUserId = resolvedPublisher ? lookup.userId : std::string{};
    const bool skipSelfForward = room->SubscriberCount() > 1U;
    std::size_t sameKindPublisherCount = 0;
    for (const auto& publisher : room->SnapshotPublishers()) {
        if (!publisher) {
            continue;
        }
        const uint32_t candidateSsrc = sourceIsVideo ? publisher->VideoSsrc() : publisher->AudioSsrc();
        if (candidateSsrc != 0U) {
            ++sameKindPublisherCount;
        }
    }

    {
        std::lock_guard<std::mutex> lock(feedbackRouteMutex_);
        for (auto it = feedbackRoutes_.begin(); it != feedbackRoutes_.end();) {
            if (it->second == sourceSsrc) {
                it = feedbackRoutes_.erase(it);
            } else {
                ++it;
            }
        }
    }

    router_.RemoveSubscribers(sourceSsrc);

    const auto subscribers = room->SnapshotSubscribers();
    for (const auto& subscriber : subscribers) {
        if (!subscriber) {
            continue;
        }
        if (skipSelfForward && !sourceUserId.empty() && subscriber->UserId() == sourceUserId) {
            continue;
        }

        const std::string& endpointText = sourceIsVideo
            ? (!subscriber->VideoEndpoint().empty()
                   ? subscriber->VideoEndpoint()
                   : (!subscriber->Endpoint().empty() ? subscriber->Endpoint() : subscriber->AudioEndpoint()))
            : (!subscriber->AudioEndpoint().empty()
                   ? subscriber->AudioEndpoint()
                   : (!subscriber->Endpoint().empty() ? subscriber->Endpoint() : subscriber->VideoEndpoint()));
        if (endpointText.empty()) {
            continue;
        }

        UdpServer::Endpoint endpoint{};
        if (!ParseEndpoint(endpointText, &endpoint)) {
            continue;
        }

        const uint32_t preferredRewrittenSsrc = sourceIsVideo ? subscriber->VideoSsrc() : subscriber->AudioSsrc();
        uint32_t rewrittenSsrc = preferredRewrittenSsrc != 0 ? preferredRewrittenSsrc : sourceSsrc;
        if (sameKindPublisherCount > 1U && preferredRewrittenSsrc != 0U) {
            rewrittenSsrc = MakeRouteRewriteSsrc(meetingId, subscriber->UserId(), sourceSsrc, sourceIsVideo);
            if (rewrittenSsrc == preferredRewrittenSsrc || rewrittenSsrc == sourceSsrc) {
                rewrittenSsrc ^= 0x01010101U;
            }
            if (rewrittenSsrc == 0U) {
                rewrittenSsrc = sourceSsrc ^ 0x01010101U;
            }
        }
        router_.AddSubscriber(sourceSsrc, rewrittenSsrc, [this, endpoint](const uint8_t* data, std::size_t len) {
            std::vector<uint8_t> packet(data, data + len);
            if (EndpointRequiresSecureMedia(endpoint) &&
                !transportRegistry_.ProtectRtpToEndpoint(endpoint, &packet)) {
                return;
            }
            udpServer_.SendTo(packet.data(), packet.size(), endpoint);
        });
        std::lock_guard<std::mutex> lock(feedbackRouteMutex_);
        feedbackRoutes_[FeedbackRouteKey(endpoint, rewrittenSsrc)] = sourceSsrc;
    }
}

void MediaIngress::RemovePublisherRoutes(uint32_t sourceSsrc) {
    if (sourceSsrc == 0) {
        return;
    }

    router_.RemoveSubscribers(sourceSsrc);
    router_.UnregisterPublisher(sourceSsrc);

    std::lock_guard<std::mutex> lock(feedbackRouteMutex_);
    for (auto it = feedbackRoutes_.begin(); it != feedbackRoutes_.end();) {
        if (it->second == sourceSsrc) {
            it = feedbackRoutes_.erase(it);
        } else {
            ++it;
        }
    }
    {
        std::lock_guard<std::mutex> endpointLock(publisherEndpointMutex_);
        publisherEndpoints_.erase(sourceSsrc);
    }
    {
        std::lock_guard<std::mutex> pliLock(pliForwardMutex_);
        pliForwardBySource_.erase(sourceSsrc);
    }
    {
        std::lock_guard<std::mutex> rembLock(rembFeedbackMutex_);
        rembFeedbackBySource_.erase(sourceSsrc);
    }
    {
        std::lock_guard<std::mutex> estimatedRembLock(estimatedRembMutex_);
        estimatedRembBySource_.erase(sourceSsrc);
    }
}

bool MediaIngress::HandleControlPacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from) {
    const auto now = std::chrono::steady_clock::now();
    bool updated = false;

    const auto senderReports = rtcpHandler_.ParseSenderReports(data, len);
    if (!senderReports.empty()) {
        for (const auto& report : senderReports) {
            RoomManager::PublisherLocation location;
            if (!ResolvePublisher(report.senderSsrc, &location)) {
                continue;
            }
            if (!TransportAllowsRtp(report.senderSsrc, from)) {
                continue;
            }

            const uint32_t compactNtp = CompactNtpFromTimestamp(report.ntpTimestamp);
            {
                std::lock_guard<std::mutex> lock(publisherTrafficMutex_);
                senderReports_[report.senderSsrc] = SenderReportLedgerEntry{
                    compactNtp,
                    now,
                };
            }
            auto receiverReport = BuildReceiverReportPacket(0U, report.senderSsrc, compactNtp, 0U);
            if (!receiverReport.empty()) {
                if (EndpointRequiresSecureMedia(from) &&
                    !transportRegistry_.ProtectRtcpToEndpoint(from, &receiverReport)) {
                    continue;
                }
                udpServer_.SendTo(receiverReport.data(), receiverReport.size(), from);
            }
            updated = true;
        }
    }

    const auto reports = rtcpHandler_.ParseReceptionReports(data, len);
    if (!reports.empty()) {
        std::lock_guard<std::mutex> lock(publisherTrafficMutex_);
        for (const auto& report : reports) {
            RoomManager::PublisherLocation location;
            if (!ResolvePublisher(report.sourceSsrc, &location)) {
                continue;
            }

            auto& counters = publisherTraffic_[PublisherTrafficKey(location.meetingId, location.publisher.userId)];
            counters.meetingId = location.meetingId;
            counters.userId = location.publisher.userId;
            counters.packetLoss = static_cast<float>(report.fractionLost) / 256.0F;
            counters.jitterMs = JitterToMs(location, report.sourceSsrc, report.jitter);
            const auto srIt = senderReports_.find(report.sourceSsrc);
            counters.rttMs = srIt == senderReports_.end()
                ? 0U
                : ComputeRttMs(srIt->second, report.lastSenderReport, report.delaySinceLastSenderReport, now);
            updated = true;
        }
    }

    const auto nackFeedback = rtcpHandler_.ParseNackFeedback(data, len);
    for (const auto& nack : nackFeedback) {
        const uint32_t sourceSsrc = ResolveNackSourceSsrc(from, nack.mediaSsrc);
        if (sourceSsrc == 0) {
            continue;
        }

        for (const uint16_t sequence : nack.lostSequences) {
            std::vector<uint8_t> retransmitPacket;
            if (!router_.GetRetransmitPacket(sourceSsrc, sequence, &retransmitPacket) || retransmitPacket.empty()) {
                continue;
            }

            if (nack.mediaSsrc != 0 && nack.mediaSsrc != sourceSsrc) {
                RewriteRtpSsrc(nack.mediaSsrc, retransmitPacket.data(), retransmitPacket.size());
            }
            if (EndpointRequiresSecureMedia(from) &&
                !transportRegistry_.ProtectRtpToEndpoint(from, &retransmitPacket)) {
                continue;
            }
            udpServer_.SendTo(retransmitPacket.data(), retransmitPacket.size(), from);
            updated = true;
        }
    }

    const auto pliFeedback = rtcpHandler_.ParsePliFeedback(data, len);
    for (const auto& pli : pliFeedback) {
        const uint32_t sourceSsrc = ResolveNackSourceSsrc(from, pli.mediaSsrc);
        if (sourceSsrc == 0) {
            continue;
        }
        if (!ShouldForwardPli(sourceSsrc, now)) {
            continue;
        }

        UdpServer::Endpoint publisherEndpoint{};
        if (!ResolvePublisherEndpoint(sourceSsrc, &publisherEndpoint)) {
            continue;
        }

        std::vector<uint8_t> pliPacket = {
            0x81, 0xCE, 0x00, 0x02,
            static_cast<uint8_t>((pli.senderSsrc >> 24) & 0xFF),
            static_cast<uint8_t>((pli.senderSsrc >> 16) & 0xFF),
            static_cast<uint8_t>((pli.senderSsrc >> 8) & 0xFF),
            static_cast<uint8_t>(pli.senderSsrc & 0xFF),
            static_cast<uint8_t>((sourceSsrc >> 24) & 0xFF),
            static_cast<uint8_t>((sourceSsrc >> 16) & 0xFF),
            static_cast<uint8_t>((sourceSsrc >> 8) & 0xFF),
            static_cast<uint8_t>(sourceSsrc & 0xFF),
        };
        if (EndpointRequiresSecureMedia(publisherEndpoint) &&
            !transportRegistry_.ProtectRtcpToEndpoint(publisherEndpoint, &pliPacket)) {
            continue;
        }
        udpServer_.SendTo(pliPacket.data(), pliPacket.size(), publisherEndpoint);
        updated = true;
    }

    const auto rembFeedback = rtcpHandler_.ParseRembFeedback(data, len);
    for (const auto& remb : rembFeedback) {
        for (const uint32_t targetSsrc : remb.targetSsrcs) {
            const uint32_t sourceSsrc = ResolveNackSourceSsrc(from, targetSsrc);
            if (sourceSsrc == 0) {
                continue;
            }

            UdpServer::Endpoint publisherEndpoint{};
            if (!ResolvePublisherEndpoint(sourceSsrc, &publisherEndpoint)) {
                continue;
            }

            uint8_t selectedExp = remb.bitrateExp;
            uint32_t selectedMantissa = remb.bitrateMantissa;
            if (!SelectAggregatedRembForSource(sourceSsrc, from, remb, &selectedExp, &selectedMantissa)) {
                continue;
            }

            std::vector<uint8_t> rembPacket = BuildRembPacket(remb.senderSsrc,
                                                              sourceSsrc,
                                                              selectedExp,
                                                              selectedMantissa,
                                                              sourceSsrc);
            if (EndpointRequiresSecureMedia(publisherEndpoint) &&
                !transportRegistry_.ProtectRtcpToEndpoint(publisherEndpoint, &rembPacket)) {
                continue;
            }
            udpServer_.SendTo(rembPacket.data(), rembPacket.size(), publisherEndpoint);
            updated = true;
        }
    }

    return updated;
}

bool MediaIngress::HandlePacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from) {
    if (IsStunPacket(data, len)) {
        return HandleStunPacket(data, len, from);
    }

    if (DtlsTransport::LooksLikeDtlsRecord(data, len)) {
        return HandleDtlsPacket(data, len, from);
    }

    if (LooksLikeRtcp(data, len)) {
        std::vector<uint8_t> controlPacket;
        const uint8_t* controlData = data;
        std::size_t controlLen = len;
        if (EndpointRequiresSecureMedia(from)) {
            controlPacket.assign(data, data + len);
            if (!transportRegistry_.UnprotectRtcpFromEndpoint(from, &controlPacket)) {
                return false;
            }
            controlData = controlPacket.data();
            controlLen = controlPacket.size();
        }

        const auto packets = rtcpHandler_.ParseCompound(controlData, controlLen);
        std::size_t consumed = 0;
        for (const auto& packet : packets) {
            consumed += packet.packetSize;
        }
        if (!packets.empty() && consumed == controlLen) {
            (void)HandleControlPacket(controlData, controlLen, from);
            return true;
        }
    }

    ParsedRtpPacket parsed;
    if (!parser_.Parse(data, len, &parsed)) {
        return false;
    }
    if (!TransportAllowsRtp(parsed.header.ssrc, from)) {
        return false;
    }

    std::vector<uint8_t> mediaPacket;
    const uint8_t* mediaData = data;
    std::size_t mediaLen = len;
    if (TransportRequiresSrtp(parsed.header.ssrc)) {
        if (!transportRegistry_.SrtpReadyForSsrcFrom(parsed.header.ssrc, from)) {
            return false;
        }
        mediaPacket.assign(data, data + len);
        if (!transportRegistry_.UnprotectRtpFromEndpoint(from, &mediaPacket)) {
            return false;
        }
        mediaData = mediaPacket.data();
        mediaLen = mediaPacket.size();
        if (!parser_.Parse(mediaData, mediaLen, &parsed)) {
            return false;
        }
    }

    RoomManager::PublisherLocation location;
    if (!ResolvePublisher(parsed.header.ssrc, &location)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> endpointLock(publisherEndpointMutex_);
        publisherEndpoints_[parsed.header.ssrc] = from;
    }

    {
        std::lock_guard<std::mutex> lock(observationMutex_);
        lastObservation_.meetingId = location.meetingId;
        lastObservation_.userId = location.publisher.userId;
        lastObservation_.ssrc = parsed.header.ssrc;
        lastObservation_.sequence = parsed.header.sequence;
        lastObservation_.timestamp = parsed.header.timestamp;
        lastObservation_.bytes = mediaLen;
    }
    {
        std::lock_guard<std::mutex> lock(publisherTrafficMutex_);
        auto& counters = publisherTraffic_[PublisherTrafficKey(location.meetingId, location.publisher.userId)];
        counters.meetingId = location.meetingId;
        counters.userId = location.publisher.userId;
        counters.packetCount += 1;
        counters.byteCount += mediaLen;
    }
    packetCount_.fetch_add(1);

    return router_.Route(mediaData, mediaLen);
}

} // namespace sfu
