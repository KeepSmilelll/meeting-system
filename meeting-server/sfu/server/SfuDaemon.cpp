#include "server/SfuDaemon.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "server/MediaIngress.h"
#include "server/RpcProtocol.h"
#include "server/RpcServer.h"
#include "server/RpcService.h"
#include "proto/pb/sfu_rpc.pb.h"
#include "room/RoomManager.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sfu {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

bool InitSockets() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void CleanupSockets() {
    WSACleanup();
}

void CloseSocket(SocketHandle socketHandle) {
    if (socketHandle != kInvalidSocket) {
        closesocket(socketHandle);
    }
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

bool InitSockets() {
    return true;
}

void CleanupSockets() {}

void CloseSocket(SocketHandle socketHandle) {
    if (socketHandle != kInvalidSocket) {
        close(socketHandle);
    }
}
#endif

uint16_t GetEnvPort(const char* key, uint16_t fallback) {
    if (key == nullptr) {
        return fallback;
    }

    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > std::numeric_limits<uint16_t>::max()) {
        return fallback;
    }
    return static_cast<uint16_t>(parsed);
}

std::string GetEnvString(const char* key, std::string fallback) {
    if (key == nullptr) {
        return fallback;
    }

    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::string(value);
}

std::string BuildAddress(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

uint32_t GetEnvUInt32(const char* key, uint32_t fallback) {
    if (key == nullptr) {
        return fallback;
    }

    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > std::numeric_limits<uint32_t>::max()) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

bool ParseEndpoint(const std::string& endpointText, sockaddr_in* out) {
    if (out == nullptr || endpointText.empty()) {
        return false;
    }

    const auto colon = endpointText.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpointText.size()) {
        return false;
    }

    const auto hostText = endpointText.substr(0, colon);
    const auto portText = endpointText.substr(colon + 1);
    char* end = nullptr;
    const unsigned long parsedPort = std::strtoul(portText.c_str(), &end, 10);
    if (end == portText.c_str() || *end != '\0' || parsedPort > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(static_cast<uint16_t>(parsedPort));
#ifdef _WIN32
    return InetPtonA(AF_INET, hostText.c_str(), &out->sin_addr) == 1;
#else
    return inet_pton(AF_INET, hostText.c_str(), &out->sin_addr) == 1;
#endif
}

bool ReadExact(SocketHandle socketHandle, uint8_t* buffer, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const auto received = recv(socketHandle, reinterpret_cast<char*>(buffer + offset),
                                   static_cast<int>(length - offset), 0);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

bool WriteExact(SocketHandle socketHandle, const uint8_t* buffer, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const auto sent = send(socketHandle, reinterpret_cast<const char*>(buffer + offset),
                               static_cast<int>(length - offset), 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool SetSocketTimeouts(SocketHandle socketHandle, uint32_t timeoutMs) {
#ifdef _WIN32
    const DWORD timeout = timeoutMs;
    const char* value = reinterpret_cast<const char*>(&timeout);
    const int length = static_cast<int>(sizeof(timeout));
    return setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, value, length) == 0 &&
           setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, value, length) == 0;
#else
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeoutMs / 1000U);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeoutMs % 1000U) * 1000U);
    return setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

bool ExchangeRpcFrame(const std::string& endpointText,
                      const RpcFrame& request,
                      RpcFrame* response) {
    if (response == nullptr) {
        return false;
    }

    if (!InitSockets()) {
        return false;
    }

    SocketHandle socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == kInvalidSocket) {
        CleanupSockets();
        return false;
    }
    if (!SetSocketTimeouts(socketHandle, 1000U)) {
        CloseSocket(socketHandle);
        CleanupSockets();
        return false;
    }

    sockaddr_in endpoint{};
    if (!ParseEndpoint(endpointText, &endpoint)) {
        CloseSocket(socketHandle);
        CleanupSockets();
        return false;
    }

    bool ok = connect(socketHandle, reinterpret_cast<sockaddr*>(&endpoint), static_cast<int>(sizeof(endpoint))) == 0;
    if (ok) {
        std::vector<uint8_t> requestBytes;
        ok = EncodeRpcFrame(request, &requestBytes) &&
             WriteExact(socketHandle, requestBytes.data(), requestBytes.size());
    }

    if (ok) {
        std::vector<uint8_t> headerBytes(kRpcHeaderSize);
        ok = ReadExact(socketHandle, headerBytes.data(), headerBytes.size());
        if (ok) {
            const uint32_t payloadSize = (static_cast<uint32_t>(headerBytes[10]) << 24U) |
                                         (static_cast<uint32_t>(headerBytes[11]) << 16U) |
                                         (static_cast<uint32_t>(headerBytes[12]) << 8U) |
                                         static_cast<uint32_t>(headerBytes[13]);
            std::vector<uint8_t> body(payloadSize);
            if (payloadSize > 0) {
                ok = ReadExact(socketHandle, body.data(), body.size());
            }
            if (ok) {
                std::vector<uint8_t> frameBytes;
                frameBytes.reserve(headerBytes.size() + body.size());
                frameBytes.insert(frameBytes.end(), headerBytes.begin(), headerBytes.end());
                frameBytes.insert(frameBytes.end(), body.begin(), body.end());
                ok = DecodeRpcFrame(frameBytes.data(), frameBytes.size(), response);
            }
        }
    }

    CloseSocket(socketHandle);
    CleanupSockets();
    return ok;
}

} // namespace

SfuDaemonConfig LoadDaemonConfigFromEnv() {
    SfuDaemonConfig config;
    config.nodeId = GetEnvString("SFU_NODE_ID", config.nodeId);
    config.advertisedHost = GetEnvString("SFU_ADVERTISED_HOST", config.advertisedHost);
    config.rpcListenPort = GetEnvPort("SFU_RPC_LISTEN_PORT", config.rpcListenPort);
    config.mediaListenPort = GetEnvPort("SFU_MEDIA_LISTEN_PORT", config.mediaListenPort);
    config.signalingReportAddress = GetEnvString("SFU_SIGNALING_REPORT_ADDR", config.signalingReportAddress);
    config.heartbeatIntervalMs = GetEnvUInt32("SFU_HEARTBEAT_INTERVAL_MS", config.heartbeatIntervalMs);
    return config;
}

SfuDaemon::SfuDaemon(SfuDaemonConfig config)
    : config_(std::move(config))
    , roomManager_(std::make_shared<RoomManager>())
    , mediaIngress_(std::make_shared<MediaIngress>(roomManager_, config_.mediaListenPort))
    , service_(std::make_shared<RpcService>(roomManager_, mediaIngress_))
    , rpcServer_(std::make_unique<RpcServer>(config_.rpcListenPort, service_, config_.advertisedHost)) {}

SfuDaemon::~SfuDaemon() {
    Stop();
}

bool SfuDaemon::Start() {
    if (!mediaIngress_ || !mediaIngress_->IsRunning() || !rpcServer_) {
        return false;
    }
    if (!rpcServer_->Start()) {
        return false;
    }

    if (service_ != nullptr) {
        service_->SetAdvertisedAddress(AdvertisedMediaAddress());
    }
    if (!config_.signalingReportAddress.empty() && !reporterRunning_.exchange(true)) {
        reporterThread_ = std::thread([this]() {
            runReporter();
        });
    }
    return true;
}

void SfuDaemon::Stop() {
    reporterRunning_.store(false);
    if (rpcServer_) {
        rpcServer_->Stop();
    }
    if (reporterThread_.joinable()) {
        reporterThread_.join();
    }
}

bool SfuDaemon::Running() const noexcept {
    return rpcServer_ != nullptr && rpcServer_->Running();
}

uint16_t SfuDaemon::RpcPort() const noexcept {
    return rpcServer_ ? rpcServer_->Port() : 0;
}

uint16_t SfuDaemon::MediaPort() const noexcept {
    return mediaIngress_ ? mediaIngress_->Port() : 0;
}

const std::string& SfuDaemon::NodeId() const noexcept {
    return config_.nodeId;
}

std::string SfuDaemon::AdvertisedMediaAddress() const {
    return BuildAddress(config_.advertisedHost, MediaPort());
}

RpcService* SfuDaemon::Service() const noexcept {
    return service_.get();
}

void SfuDaemon::runReporter() {
    while (reporterRunning_.load()) {
        (void)sendNodeStatusReport();
        (void)sendQualityReports();

        const auto sleepSlice = std::chrono::milliseconds(100);
        const auto totalSleep = std::chrono::milliseconds(
            config_.heartbeatIntervalMs == 0 ? 5000U : config_.heartbeatIntervalMs);
        auto waited = std::chrono::milliseconds::zero();
        while (reporterRunning_.load() && waited < totalSleep) {
            std::this_thread::sleep_for(sleepSlice);
            waited += sleepSlice;
        }
    }
}

bool SfuDaemon::sendNodeStatusReport() const {
    if (service_ == nullptr || config_.signalingReportAddress.empty()) {
        return false;
    }

    sfu_rpc::GetNodeStatusReq statusReq;
    statusReq.set_probe(true);
    sfu_rpc::GetNodeStatusRsp statusRsp;
    if (!service_->HandleGetNodeStatus(statusReq, &statusRsp) || !statusRsp.success()) {
        return false;
    }

    sfu_rpc::ReportNodeStatusReq reportReq;
    reportReq.set_node_id(config_.nodeId);
    reportReq.set_rpc_address(BuildAddress(config_.advertisedHost, RpcPort()));
    reportReq.set_sfu_address(AdvertisedMediaAddress());
    reportReq.set_max_meetings(0);
    reportReq.set_media_port(statusRsp.media_port());
    reportReq.set_room_count(statusRsp.room_count());
    reportReq.set_publisher_count(statusRsp.publisher_count());
    reportReq.set_packet_count(statusRsp.packet_count());

    std::string payload;
    if (!reportReq.SerializeToString(&payload)) {
        return false;
    }

    RpcFrame request;
    request.method = RpcMethod::kReportNodeStatus;
    request.kind = RpcFrameKind::kRequest;
    request.payload.assign(payload.begin(), payload.end());

    RpcFrame response;
    if (!ExchangeRpcFrame(config_.signalingReportAddress, request, &response) ||
        response.method != RpcMethod::kReportNodeStatus ||
        response.kind != RpcFrameKind::kResponse ||
        response.status != 0) {
        return false;
    }

    sfu_rpc::ReportNodeStatusRsp reportRsp;
    return reportRsp.ParseFromArray(response.payload.data(), static_cast<int>(response.payload.size())) &&
           reportRsp.success();
}

bool SfuDaemon::sendQualityReports() const {
    if (mediaIngress_ == nullptr || config_.signalingReportAddress.empty()) {
        return false;
    }

    const auto snapshots = mediaIngress_->SnapshotPublisherTraffic();
    const auto now = std::chrono::steady_clock::now();
    std::unordered_map<std::string, QualityReportBaseline> nextBaselines;
    nextBaselines.reserve(snapshots.size());

    bool anySent = false;
    std::lock_guard<std::mutex> lock(qualityReportMutex_);
    for (const auto& snapshot : snapshots) {
        const auto key = QualityReportKey(snapshot.meetingId, snapshot.userId);
        uint32_t measuredBitrateKbps = 0;

        const auto prev = qualityBaselines_.find(key);
        if (prev != qualityBaselines_.end() &&
            snapshot.byteCount >= prev->second.byteCount &&
            now > prev->second.observedAt) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - prev->second.observedAt).count();
            if (elapsedMs > 0) {
                const auto deltaBytes = snapshot.byteCount - prev->second.byteCount;
                measuredBitrateKbps = static_cast<uint32_t>((deltaBytes * 8ULL + static_cast<uint64_t>(elapsedMs / 2)) /
                                                            static_cast<uint64_t>(elapsedMs));
            }
        }

        auto estimatorIt = qualityEstimators_.find(key);
        if (estimatorIt == qualityEstimators_.end()) {
            estimatorIt = qualityEstimators_.emplace(key, std::make_unique<bwe::BandwidthEstimator>()).first;
        }
        uint32_t recommendedBitrateKbps = measuredBitrateKbps;
        if (estimatorIt->second) {
            recommendedBitrateKbps = estimatorIt->second->Update(bwe::BandwidthEstimator::Sample{
                snapshot.packetLoss,
                snapshot.rttMs,
                snapshot.jitterMs,
                measuredBitrateKbps,
            });
        }

        nextBaselines.emplace(key, QualityReportBaseline{snapshot.packetCount, snapshot.byteCount, now});
        if (mediaIngress_ != nullptr && recommendedBitrateKbps > 0) {
            (void)mediaIngress_->SendEstimatedRembToPublisher(snapshot.meetingId,
                                                              snapshot.userId,
                                                              recommendedBitrateKbps);
        }
        anySent = sendQualityReportFrame(snapshot.meetingId,
                                         snapshot.userId,
                                         recommendedBitrateKbps,
                                         snapshot.packetLoss,
                                         snapshot.rttMs,
                                         snapshot.jitterMs) || anySent;
    }

    for (auto it = qualityEstimators_.begin(); it != qualityEstimators_.end();) {
        if (nextBaselines.find(it->first) == nextBaselines.end()) {
            it = qualityEstimators_.erase(it);
            continue;
        }
        ++it;
    }

    qualityBaselines_.swap(nextBaselines);
    return anySent;
}

bool SfuDaemon::sendQualityReportFrame(const std::string& meetingId,
                                       const std::string& userId,
                                       uint32_t bitrateKbps,
                                       float packetLoss,
                                       uint32_t rttMs,
                                       uint32_t jitterMs) const {
    if (config_.signalingReportAddress.empty() || meetingId.empty() || userId.empty()) {
        return false;
    }

    sfu_rpc::QualityReport report;
    report.set_meeting_id(meetingId);
    report.set_user_id(userId);
    report.set_packet_loss(packetLoss);
    report.set_rtt_ms(rttMs);
    report.set_jitter_ms(jitterMs);
    report.set_bitrate_kbps(bitrateKbps);

    std::string payload;
    if (!report.SerializeToString(&payload)) {
        return false;
    }

    RpcFrame request;
    request.method = RpcMethod::kQualityReport;
    request.kind = RpcFrameKind::kRequest;
    request.payload.assign(payload.begin(), payload.end());

    RpcFrame response;
    return ExchangeRpcFrame(config_.signalingReportAddress, request, &response) &&
           response.method == RpcMethod::kQualityReport &&
           response.kind == RpcFrameKind::kResponse &&
           response.status == 0;
}

std::string SfuDaemon::QualityReportKey(const std::string& meetingId, const std::string& userId) {
    return meetingId + "\n" + userId;
}

} // namespace sfu

