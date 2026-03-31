#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "room/RoomManager.h"
#include "rtp/RtpParser.h"
#include "rtp/RtpRouter.h"
#include "server/UdpServer.h"

namespace sfu {

class MediaIngress final {
public:
    struct PacketObservation {
        std::string meetingId;
        std::string userId;
        uint32_t ssrc{0};
        uint16_t sequence{0};
        uint32_t timestamp{0};
        std::size_t bytes{0};
    };

    explicit MediaIngress(std::shared_ptr<RoomManager> roomManager, uint16_t listenPort = 0, std::size_t nackCapacity = 500);
    ~MediaIngress();

    uint16_t Port() const noexcept;
    bool IsRunning() const noexcept;

    bool BindPublisher(const std::string& meetingId,
                       const std::string& userId,
                       uint32_t audioSsrc,
                       uint32_t videoSsrc);
    bool RemovePublisher(const std::string& meetingId, const std::string& userId);

    bool ResolvePublisher(uint32_t ssrc, RoomManager::PublisherLocation* out) const;
    std::size_t PacketCount() const noexcept;
    bool LastObservation(PacketObservation* out) const;

private:
    static bool ParseEndpoint(const std::string& endpointText, UdpServer::Endpoint* endpoint);

    void RefreshRoomRoutes(const std::string& meetingId);
    void RefreshPublisherRoutes(const std::string& meetingId, uint32_t sourceSsrc);
    void RemovePublisherRoutes(uint32_t sourceSsrc);
    bool HandlePacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from);

    UdpServer udpServer_;
    std::shared_ptr<RoomManager> roomManager_;
    RtpRouter router_;
    RtpParser parser_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> packetCount_{0};
    mutable std::mutex observationMutex_;
    PacketObservation lastObservation_;
};

} // namespace sfu
