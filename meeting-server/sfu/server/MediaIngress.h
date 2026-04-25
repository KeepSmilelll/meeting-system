#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "room/RoomManager.h"
#include "rtp/RtcpHandler.h"
#include "rtp/RtpParser.h"
#include "rtp/RtpRouter.h"
#include "server/DtlsContext.h"
#include "server/TransportRegistry.h"
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

    struct PublisherTrafficSnapshot {
        std::string meetingId;
        std::string userId;
        uint64_t packetCount{0};
        uint64_t byteCount{0};
        float packetLoss{0.0F};
        uint32_t jitterMs{0};
        uint32_t rttMs{0};
    };

    struct TransportSetupResult {
        bool success{false};
        std::string serverIceUfrag;
        std::string serverIcePwd;
        std::string serverDtlsFingerprint;
        std::vector<std::string> serverCandidates;
        uint32_t assignedAudioSsrc{0};
        uint32_t assignedVideoSsrc{0};
    };

    explicit MediaIngress(std::shared_ptr<RoomManager> roomManager, uint16_t listenPort = 0, std::size_t nackCapacity = 500);
    ~MediaIngress();

    uint16_t Port() const noexcept;
    bool IsRunning() const noexcept;

    bool BindPublisher(const std::string& meetingId,
                       const std::string& userId,
                       uint32_t audioSsrc,
                       uint32_t videoSsrc);
    bool SetupTransport(const std::string& meetingId,
                        const std::string& userId,
                        bool publishAudio,
                        bool publishVideo,
                        const std::string& clientIceUfrag,
                        const std::string& clientIcePwd,
                        const std::string& clientDtlsFingerprint,
                        const std::vector<std::string>& clientCandidates,
                        const std::string& advertisedAddress,
                        TransportSetupResult* result);
    bool TrickleIceCandidate(const std::string& meetingId,
                             const std::string& userId,
                             const std::string& candidate,
                             const std::string& sdpMid,
                             bool endOfCandidates);
    bool CloseTransport(const std::string& meetingId, const std::string& userId);
    bool BindSubscriber(const std::string& meetingId,
                        const std::string& userId,
                        uint32_t audioSsrc,
                        uint32_t videoSsrc);
    bool RemovePublisher(const std::string& meetingId, const std::string& userId);
    bool RemoveSubscriber(const std::string& meetingId, const std::string& userId);

    bool ResolvePublisher(uint32_t ssrc, RoomManager::PublisherLocation* out) const;
    std::size_t PacketCount() const noexcept;
    bool LastObservation(PacketObservation* out) const;
    std::vector<PublisherTrafficSnapshot> SnapshotPublisherTraffic() const;
    bool SendEstimatedRembToPublisher(const std::string& meetingId,
                                      const std::string& userId,
                                      uint32_t bitrateKbps);

private:
    struct PublisherTrafficCounters {
        std::string meetingId;
        std::string userId;
        uint64_t packetCount{0};
        uint64_t byteCount{0};
        float packetLoss{0.0F};
        uint32_t jitterMs{0};
        uint32_t rttMs{0};
    };

    struct SenderReportLedgerEntry {
        uint32_t compactNtp{0};
        std::chrono::steady_clock::time_point observedAt{};
    };

    struct RembFeedbackSample {
        uint8_t bitrateExp{0};
        uint32_t bitrateMantissa{0};
        uint32_t bitrateBps{0};
        std::chrono::steady_clock::time_point observedAt{};
    };

    struct EstimatedRembState {
        uint32_t bitrateBps{0};
        std::chrono::steady_clock::time_point sentAt{};
    };

    static bool ParseEndpoint(const std::string& endpointText, UdpServer::Endpoint* endpoint);
    static std::string HostFromAdvertisedAddress(const std::string& advertisedAddress);
    static std::string MakeHostCandidate(const std::string& host, uint16_t port);
    static std::string MakeRandomIceToken(std::size_t bytes);
    static uint32_t MakeRandomSsrc();
    static bool IsStunPacket(const uint8_t* data, std::size_t len);
    static std::string ParseStunUsername(const uint8_t* data, std::size_t len);
    static std::vector<uint8_t> BuildStunBindingResponse(const uint8_t* request, std::size_t len, const UdpServer::Endpoint& from);
    static bool LooksLikeRtcp(const uint8_t* data, std::size_t len);
    static std::string PublisherTrafficKey(const std::string& meetingId, const std::string& userId);
    static std::string EndpointKey(const UdpServer::Endpoint& endpoint);
    static std::string FeedbackRouteKey(const UdpServer::Endpoint& endpoint, uint32_t mediaSsrc);
    static void RewriteRtpSsrc(uint32_t ssrc, uint8_t* packet, std::size_t len);
    static uint32_t MakeRouteRewriteSsrc(const std::string& meetingId,
                                         const std::string& subscriberUserId,
                                         uint32_t sourceSsrc,
                                         bool video);
    static uint32_t JitterToMs(const RoomManager::PublisherLocation& location, uint32_t sourceSsrc, uint32_t jitter);
    static uint32_t CompactNtpFromTimestamp(uint64_t ntpTimestamp);
    static uint32_t CompactNtpFromElapsed(std::chrono::steady_clock::duration elapsed);
    static uint32_t CompactNtpToMs(uint32_t compactNtp);
    static uint32_t ComputeRttMs(const SenderReportLedgerEntry& entry,
                                 uint32_t lastSenderReport,
                                 uint32_t delaySinceLastSenderReport,
                                 std::chrono::steady_clock::time_point now);
    bool ShouldForwardPli(uint32_t sourceSsrc, std::chrono::steady_clock::time_point now);
    bool SelectAggregatedRembForSource(uint32_t sourceSsrc,
                                       const UdpServer::Endpoint& from,
                                       const RtcpRembFeedback& remb,
                                       uint8_t* outBitrateExp,
                                       uint32_t* outBitrateMantissa);
    bool HandleStunPacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from);
    bool HandleDtlsPacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from);
    bool TransportAllowsRtp(uint32_t ssrc, const UdpServer::Endpoint& from) const;
    bool TransportRequiresSrtp(uint32_t ssrc) const;
    bool EndpointRequiresSecureMedia(const UdpServer::Endpoint& from) const;
    uint32_t ResolveNackSourceSsrc(const UdpServer::Endpoint& from, uint32_t mediaSsrc) const;
    bool ResolvePublisherEndpoint(uint32_t sourceSsrc, UdpServer::Endpoint* out) const;

    void RefreshRoomRoutes(const std::string& meetingId);
    void RefreshPublisherRoutes(const std::string& meetingId, uint32_t sourceSsrc);
    void RemovePublisherRoutes(uint32_t sourceSsrc);
    bool HandleControlPacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from);
    bool HandlePacket(const uint8_t* data, std::size_t len, const UdpServer::Endpoint& from);

    UdpServer udpServer_;
    std::shared_ptr<RoomManager> roomManager_;
    RtcpHandler rtcpHandler_;
    RtpRouter router_;
    RtpParser parser_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> packetCount_{0};
    mutable std::mutex observationMutex_;
    PacketObservation lastObservation_;
    mutable std::mutex publisherTrafficMutex_;
    mutable std::unordered_map<std::string, PublisherTrafficCounters> publisherTraffic_;
    mutable std::unordered_map<uint32_t, SenderReportLedgerEntry> senderReports_;
    mutable std::mutex feedbackRouteMutex_;
    std::unordered_map<std::string, uint32_t> feedbackRoutes_;
    mutable std::mutex publisherEndpointMutex_;
    std::unordered_map<uint32_t, UdpServer::Endpoint> publisherEndpoints_;
    mutable std::mutex pliForwardMutex_;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> pliForwardBySource_;
    mutable std::mutex rembFeedbackMutex_;
    std::unordered_map<uint32_t, std::unordered_map<std::string, RembFeedbackSample>> rembFeedbackBySource_;
    mutable std::mutex estimatedRembMutex_;
    std::unordered_map<uint32_t, EstimatedRembState> estimatedRembBySource_;
    TransportRegistry transportRegistry_;
    DtlsContext dtlsContext_;
};

} // namespace sfu

