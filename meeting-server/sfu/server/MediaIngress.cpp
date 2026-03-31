#include "server/MediaIngress.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

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
    if (meetingId.empty() || userId.empty() || (audioSsrc == 0 && videoSsrc == 0) || !roomManager_) {
        return false;
    }

    const auto room = roomManager_->GetRoomShared(meetingId);
    if (!room) {
        return false;
    }

    const auto publisher = std::make_shared<Publisher>(userId, audioSsrc, videoSsrc);
    if (!room->AddPublisher(publisher)) {
        return false;
    }

    if (audioSsrc != 0) {
        router_.RegisterPublisher(audioSsrc);
    }
    if (videoSsrc != 0) {
        router_.RegisterPublisher(videoSsrc);
    }

    RefreshRoomRoutes(meetingId);
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

    router_.RemoveSubscribers(sourceSsrc);

    const auto subscribers = room->SnapshotSubscribers();
    for (const auto& subscriber : subscribers) {
        if (!subscriber || subscriber->Endpoint().empty()) {
            continue;
        }

        UdpServer::Endpoint endpoint{};
        if (!ParseEndpoint(subscriber->Endpoint(), &endpoint)) {
            continue;
        }

        const uint32_t rewrittenSsrc = subscriber->AudioSsrc() != 0 ? subscriber->AudioSsrc() : sourceSsrc;
        router_.AddSubscriber(sourceSsrc, rewrittenSsrc, [this, endpoint](const uint8_t* data, std::size_t len) {
            udpServer_.SendTo(data, len, endpoint);
        });
    }
}

void MediaIngress::RemovePublisherRoutes(uint32_t sourceSsrc) {
    if (sourceSsrc == 0) {
        return;
    }

    router_.RemoveSubscribers(sourceSsrc);
    router_.UnregisterPublisher(sourceSsrc);
}

bool MediaIngress::HandlePacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from) {
    (void)from;

    ParsedRtpPacket parsed;
    if (!parser_.Parse(data, len, &parsed)) {
        return false;
    }

    RoomManager::PublisherLocation location;
    if (!ResolvePublisher(parsed.header.ssrc, &location)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(observationMutex_);
        lastObservation_.meetingId = location.meetingId;
        lastObservation_.userId = location.publisher.userId;
        lastObservation_.ssrc = parsed.header.ssrc;
        lastObservation_.sequence = parsed.header.sequence;
        lastObservation_.timestamp = parsed.header.timestamp;
        lastObservation_.bytes = len;
    }
    packetCount_.fetch_add(1);

    return router_.Route(data, len);
}

} // namespace sfu
