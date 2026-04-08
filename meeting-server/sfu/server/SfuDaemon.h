#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "bwe/BandwidthEstimator.h"

namespace sfu {

class MediaIngress;
class RoomManager;
class RpcServer;
class RpcService;

struct SfuDaemonConfig final {
    std::string nodeId{"sfu-node-01"};
    std::string advertisedHost{"127.0.0.1"};
    uint16_t rpcListenPort{9000};
    uint16_t mediaListenPort{10000};
    std::string signalingReportAddress{};
    uint32_t heartbeatIntervalMs{5000};
};

SfuDaemonConfig LoadDaemonConfigFromEnv();

class SfuDaemon final {
public:
    explicit SfuDaemon(SfuDaemonConfig config);
    ~SfuDaemon();

    bool Start();
    void Stop();

    bool Running() const noexcept;
    uint16_t RpcPort() const noexcept;
    uint16_t MediaPort() const noexcept;
    const std::string& NodeId() const noexcept;
    std::string AdvertisedMediaAddress() const;
    RpcService* Service() const noexcept;

private:
    struct QualityReportBaseline {
        uint64_t packetCount{0};
        uint64_t byteCount{0};
        std::chrono::steady_clock::time_point observedAt{};
    };

    SfuDaemonConfig config_;
    std::shared_ptr<RoomManager> roomManager_;
    std::shared_ptr<MediaIngress> mediaIngress_;
    std::shared_ptr<RpcService> service_;
    std::unique_ptr<RpcServer> rpcServer_;
    std::atomic<bool> reporterRunning_{false};
    std::thread reporterThread_;
    mutable std::mutex qualityReportMutex_;
    mutable std::unordered_map<std::string, QualityReportBaseline> qualityBaselines_;
    mutable std::unordered_map<std::string, std::unique_ptr<bwe::BandwidthEstimator>> qualityEstimators_;

    void runReporter();
    bool sendNodeStatusReport() const;
    bool sendQualityReports() const;
    bool sendQualityReportFrame(const std::string& meetingId,
                                const std::string& userId,
                                uint32_t bitrateKbps,
                                float packetLoss,
                                uint32_t rttMs,
                                uint32_t jitterMs) const;
    static std::string QualityReportKey(const std::string& meetingId, const std::string& userId);
};

} // namespace sfu
