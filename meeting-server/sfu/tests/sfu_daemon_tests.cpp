#include "server/SfuDaemon.h"
#include "server/RpcProtocol.h"
#include "server/RpcService.h"

#include "proto/pb/sfu_rpc.pb.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

struct ScopedEnvVar final {
    std::string key;
    std::string originalValue;
    bool hadOriginal{false};

    ScopedEnvVar(std::string envKey, std::string value)
        : key(std::move(envKey)) {
        if (const char* current = std::getenv(key.c_str())) {
            originalValue = current;
            hadOriginal = true;
        }
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }

    ~ScopedEnvVar() {
        if (hadOriginal) {
#ifdef _WIN32
            _putenv_s(key.c_str(), originalValue.c_str());
#else
            setenv(key.c_str(), originalValue.c_str(), 1);
#endif
            return;
        }
#ifdef _WIN32
        _putenv_s(key.c_str(), "");
#else
        unsetenv(key.c_str());
#endif
    }
};

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

bool CreateBoundUdpSocket(SocketHandle* outSocket, uint16_t* outPort) {
    if (outSocket == nullptr || outPort == nullptr) {
        return false;
    }

    SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == kInvalidSocket) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
#else
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        CloseSocket(socketHandle);
        return false;
    }
#endif

    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) != 0) {
        CloseSocket(socketHandle);
        return false;
    }

    sockaddr_in localAddr{};
#ifdef _WIN32
    int localAddrLen = static_cast<int>(sizeof(localAddr));
#else
    socklen_t localAddrLen = static_cast<socklen_t>(sizeof(localAddr));
#endif
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&localAddr), &localAddrLen) != 0) {
        CloseSocket(socketHandle);
        return false;
    }

    *outPort = ntohs(localAddr.sin_port);
    *outSocket = socketHandle;
    return true;
}

bool SendUdpPacket(SocketHandle socketHandle, uint16_t port, const uint8_t* data, std::size_t len) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
#else
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        return false;
    }
#endif

    const auto sent = sendto(socketHandle,
                             reinterpret_cast<const char*>(data),
                             static_cast<int>(len),
                             0,
                             reinterpret_cast<sockaddr*>(&addr),
                             static_cast<int>(sizeof(addr)));
    return sent == static_cast<int>(len);
}

bool ReceiveUdpPacket(SocketHandle socketHandle, std::vector<uint8_t>* out, int timeoutMs = 1000) {
    if (out == nullptr) {
        return false;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef _WIN32
    const int rc = select(0, &readSet, nullptr, nullptr, &tv);
#else
    const int rc = select(socketHandle + 1, &readSet, nullptr, nullptr, &tv);
#endif
    if (rc <= 0 || !FD_ISSET(socketHandle, &readSet)) {
        return false;
    }

    std::vector<uint8_t> buffer(2048);
    sockaddr_in from{};
#ifdef _WIN32
    int fromLen = static_cast<int>(sizeof(from));
#else
    socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
#endif
    const auto received = recvfrom(socketHandle,
                                   reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(buffer.size()),
                                   0,
                                   reinterpret_cast<sockaddr*>(&from),
                                   &fromLen);
    if (received <= 0) {
        return false;
    }

    buffer.resize(static_cast<std::size_t>(received));
    *out = std::move(buffer);
    return true;
}

bool TestLoadDaemonConfigFromEnv() {
    ScopedEnvVar nodeId("SFU_NODE_ID", "sfu-node-test");
    ScopedEnvVar advertisedHost("SFU_ADVERTISED_HOST", "10.0.0.8");
    ScopedEnvVar rpcPort("SFU_RPC_LISTEN_PORT", "19000");
    ScopedEnvVar mediaPort("SFU_MEDIA_LISTEN_PORT", "20000");
    ScopedEnvVar reportAddr("SFU_SIGNALING_REPORT_ADDR", "127.0.0.1:19100");
    ScopedEnvVar heartbeatMs("SFU_HEARTBEAT_INTERVAL_MS", "1500");

    const sfu::SfuDaemonConfig config = sfu::LoadDaemonConfigFromEnv();
    return config.nodeId == "sfu-node-test" &&
           config.advertisedHost == "10.0.0.8" &&
           config.rpcListenPort == 19000 &&
           config.mediaListenPort == 20000 &&
           config.signalingReportAddress == "127.0.0.1:19100" &&
           config.heartbeatIntervalMs == 1500;
}

bool TestDaemonLifecycle() {
    sfu::SfuDaemon daemon({
        "sfu-node-lifecycle",
        "127.0.0.1",
        0,
        0,
    });

    if (!daemon.Start()) {
        std::cerr << "daemon.Start failed\n";
        return false;
    }

    if (!daemon.Running() || daemon.RpcPort() == 0 || daemon.MediaPort() == 0) {
        std::cerr << "daemon should expose runtime ports\n";
        daemon.Stop();
        return false;
    }

    const std::string expectedAddress = "127.0.0.1:" + std::to_string(daemon.MediaPort());
    if (daemon.AdvertisedMediaAddress() != expectedAddress) {
        std::cerr << "unexpected advertised media address: " << daemon.AdvertisedMediaAddress() << "\n";
        daemon.Stop();
        return false;
    }

    auto* service = daemon.Service();
    if (service == nullptr || service->AdvertisedAddress() != expectedAddress) {
        std::cerr << "service should advertise media address\n";
        daemon.Stop();
        return false;
    }

    sfu_rpc::GetNodeStatusReq req;
    sfu_rpc::GetNodeStatusRsp rsp;
    if (!service->HandleGetNodeStatus(req, &rsp)) {
        std::cerr << "HandleGetNodeStatus failed\n";
        daemon.Stop();
        return false;
    }

    daemon.Stop();
    return rsp.success() &&
           rsp.sfu_address() == expectedAddress &&
           rsp.media_port() == daemon.MediaPort();
}

bool TestDaemonReportsNodeStatusToSignaling() {
    if (!InitSockets()) {
        std::cerr << "socket init failed\n";
        return false;
    }

    SocketHandle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        CleanupSockets();
        return false;
    }

    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), static_cast<int>(sizeof(reuse)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
        CloseSocket(listener);
        CleanupSockets();
        return false;
    }

    sockaddr_in localAddr{};
#ifdef _WIN32
    int localAddrLen = static_cast<int>(sizeof(localAddr));
#else
    socklen_t localAddrLen = static_cast<socklen_t>(sizeof(localAddr));
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&localAddr), &localAddrLen) != 0) {
        CloseSocket(listener);
        CleanupSockets();
        return false;
    }

    const uint16_t reportPort = ntohs(localAddr.sin_port);
    std::atomic<bool> received{false};
    std::string nodeId;
    std::string rpcAddress;
    std::string sfuAddress;

    std::thread serverThread([&]() {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int clientAddrLen = static_cast<int>(sizeof(clientAddr));
#else
        socklen_t clientAddrLen = static_cast<socklen_t>(sizeof(clientAddr));
#endif
        const auto client = accept(listener, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (client == kInvalidSocket) {
            return;
        }

        std::vector<uint8_t> headerBytes(sfu::kRpcHeaderSize);
        if (!ReadExact(client, headerBytes.data(), headerBytes.size())) {
            CloseSocket(client);
            return;
        }
        const uint32_t payloadSize = (static_cast<uint32_t>(headerBytes[10]) << 24U) |
                                     (static_cast<uint32_t>(headerBytes[11]) << 16U) |
                                     (static_cast<uint32_t>(headerBytes[12]) << 8U) |
                                     static_cast<uint32_t>(headerBytes[13]);
        std::vector<uint8_t> payload(payloadSize);
        if (payloadSize > 0 && !ReadExact(client, payload.data(), payload.size())) {
            CloseSocket(client);
            return;
        }

        std::vector<uint8_t> frameBytes;
        frameBytes.reserve(headerBytes.size() + payload.size());
        frameBytes.insert(frameBytes.end(), headerBytes.begin(), headerBytes.end());
        frameBytes.insert(frameBytes.end(), payload.begin(), payload.end());

        sfu::RpcFrame frame;
        if (!sfu::DecodeRpcFrame(frameBytes.data(), frameBytes.size(), &frame) ||
            frame.method != sfu::RpcMethod::kReportNodeStatus ||
            frame.kind != sfu::RpcFrameKind::kRequest) {
            CloseSocket(client);
            return;
        }

        sfu_rpc::ReportNodeStatusReq req;
        if (!req.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
            CloseSocket(client);
            return;
        }

        nodeId = req.node_id();
        rpcAddress = req.rpc_address();
        sfuAddress = req.sfu_address();
        received.store(true);

        sfu_rpc::ReportNodeStatusRsp rsp;
        rsp.set_success(true);
        std::string rspBytes;
        if (!rsp.SerializeToString(&rspBytes)) {
            CloseSocket(client);
            return;
        }

        sfu::RpcFrame response;
        response.method = sfu::RpcMethod::kReportNodeStatus;
        response.kind = sfu::RpcFrameKind::kResponse;
        response.status = 0;
        response.payload.assign(rspBytes.begin(), rspBytes.end());

        std::vector<uint8_t> encoded;
        if (sfu::EncodeRpcFrame(response, &encoded)) {
            (void)WriteExact(client, encoded.data(), encoded.size());
        }
        CloseSocket(client);
    });

    sfu::SfuDaemon daemon({
        "sfu-node-active",
        "127.0.0.1",
        0,
        0,
        "127.0.0.1:" + std::to_string(reportPort),
        100,
    });

    const bool started = daemon.Start();
    for (int i = 0; i < 30 && !received.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    daemon.Stop();
    CloseSocket(listener);
    if (serverThread.joinable()) {
        serverThread.join();
    }
    CleanupSockets();

    return started &&
           received.load() &&
           nodeId == "sfu-node-active" &&
           rpcAddress == "127.0.0.1:" + std::to_string(daemon.RpcPort()) &&
           sfuAddress == "127.0.0.1:" + std::to_string(daemon.MediaPort());
}

bool TestDaemonReportsPublisherQualityToSignaling() {
    if (!InitSockets()) {
        std::cerr << "socket init failed\n";
        return false;
    }

    SocketHandle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        CleanupSockets();
        return false;
    }

    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), static_cast<int>(sizeof(reuse)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
        CloseSocket(listener);
        CleanupSockets();
        return false;
    }

    sockaddr_in localAddr{};
#ifdef _WIN32
    int localAddrLen = static_cast<int>(sizeof(localAddr));
#else
    socklen_t localAddrLen = static_cast<socklen_t>(sizeof(localAddr));
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&localAddr), &localAddrLen) != 0) {
        CloseSocket(listener);
        CleanupSockets();
        return false;
    }

    const uint16_t reportPort = ntohs(localAddr.sin_port);
    std::atomic<bool> receivedNode{false};
    std::atomic<bool> receivedPositiveQuality{false};
    std::string qualityMeetingId;
    std::string qualityUserId;
    uint32_t qualityBitrateKbps = 0;
    float qualityPacketLoss = 0.0F;
    uint32_t qualityRttMs = 0;
    uint32_t qualityJitterMs = 0;
    bool receivedRemb = false;
    uint32_t rembTargetSsrc = 0;
    uint32_t rembBitrateBps = 0;

    std::thread serverThread([&]() {
        while (true) {
            sockaddr_in clientAddr{};
#ifdef _WIN32
            int clientAddrLen = static_cast<int>(sizeof(clientAddr));
#else
            socklen_t clientAddrLen = static_cast<socklen_t>(sizeof(clientAddr));
#endif
            const auto client = accept(listener, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
            if (client == kInvalidSocket) {
                return;
            }

            std::vector<uint8_t> headerBytes(sfu::kRpcHeaderSize);
            if (!ReadExact(client, headerBytes.data(), headerBytes.size())) {
                CloseSocket(client);
                continue;
            }
            const uint32_t payloadSize = (static_cast<uint32_t>(headerBytes[10]) << 24U) |
                                         (static_cast<uint32_t>(headerBytes[11]) << 16U) |
                                         (static_cast<uint32_t>(headerBytes[12]) << 8U) |
                                         static_cast<uint32_t>(headerBytes[13]);
            std::vector<uint8_t> payload(payloadSize);
            if (payloadSize > 0 && !ReadExact(client, payload.data(), payload.size())) {
                CloseSocket(client);
                continue;
            }

            std::vector<uint8_t> frameBytes;
            frameBytes.reserve(headerBytes.size() + payload.size());
            frameBytes.insert(frameBytes.end(), headerBytes.begin(), headerBytes.end());
            frameBytes.insert(frameBytes.end(), payload.begin(), payload.end());

            sfu::RpcFrame frame;
            if (!sfu::DecodeRpcFrame(frameBytes.data(), frameBytes.size(), &frame) ||
                frame.kind != sfu::RpcFrameKind::kRequest) {
                CloseSocket(client);
                continue;
            }

            sfu::RpcFrame response;
            response.method = frame.method;
            response.kind = sfu::RpcFrameKind::kResponse;
            response.status = 0;

            if (frame.method == sfu::RpcMethod::kReportNodeStatus) {
                sfu_rpc::ReportNodeStatusReq req;
                if (req.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
                    receivedNode.store(true);
                    sfu_rpc::ReportNodeStatusRsp rsp;
                    rsp.set_success(true);
                    std::string rspBytes;
                    if (rsp.SerializeToString(&rspBytes)) {
                        response.payload.assign(rspBytes.begin(), rspBytes.end());
                    }
                }
            } else if (frame.method == sfu::RpcMethod::kQualityReport) {
                sfu_rpc::QualityReport req;
                if (req.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
                    qualityMeetingId = req.meeting_id();
                    qualityUserId = req.user_id();
                    qualityBitrateKbps = req.bitrate_kbps();
                    qualityPacketLoss = req.packet_loss();
                    qualityRttMs = req.rtt_ms();
                    qualityJitterMs = req.jitter_ms();
                    if (req.bitrate_kbps() > 0 && req.packet_loss() > 0.0F && req.rtt_ms() > 0 && req.jitter_ms() > 0) {
                        receivedPositiveQuality.store(true);
                    }
                }
            }

            std::vector<uint8_t> encoded;
            if (sfu::EncodeRpcFrame(response, &encoded)) {
                (void)WriteExact(client, encoded.data(), encoded.size());
            }
            CloseSocket(client);
        }
    });

    sfu::SfuDaemon daemon({
        "sfu-node-quality",
        "127.0.0.1",
        0,
        0,
        "127.0.0.1:" + std::to_string(reportPort),
        100,
    });

    const bool started = daemon.Start();
    if (!started) {
        CloseSocket(listener);
        if (serverThread.joinable()) {
            serverThread.join();
        }
        CleanupSockets();
        return false;
    }

    auto* service = daemon.Service();
    if (service == nullptr) {
        daemon.Stop();
        CloseSocket(listener);
        if (serverThread.joinable()) {
            serverThread.join();
        }
        CleanupSockets();
        return false;
    }

    sfu_rpc::CreateRoomReq createReq;
    createReq.set_meeting_id("room-quality");
    createReq.set_max_publishers(2);
    sfu_rpc::CreateRoomRsp createRsp;
    if (!service->HandleCreateRoom(createReq, &createRsp) || !createRsp.success()) {
        daemon.Stop();
        CloseSocket(listener);
        if (serverThread.joinable()) {
            serverThread.join();
        }
        CleanupSockets();
        return false;
    }

    sfu_rpc::AddPublisherReq addReq;
    addReq.set_meeting_id("room-quality");
    addReq.set_user_id("alice");
    addReq.set_audio_ssrc(0x11111111U);
    sfu_rpc::AddPublisherRsp addRsp;
    if (!service->HandleAddPublisher(addReq, &addRsp) || !addRsp.success() || daemon.MediaPort() == 0) {
        daemon.Stop();
        CloseSocket(listener);
        if (serverThread.joinable()) {
            serverThread.join();
        }
        CleanupSockets();
        return false;
    }

    SocketHandle sendSocket = kInvalidSocket;
    uint16_t senderPort = 0;
    if (!CreateBoundUdpSocket(&sendSocket, &senderPort)) {
        daemon.Stop();
        CloseSocket(listener);
        if (serverThread.joinable()) {
            serverThread.join();
        }
        CleanupSockets();
        return false;
    }

    const std::vector<uint8_t> rtpPacket = {
        0x80, 0x60,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD, 0xEF
    };

    (void)SendUdpPacket(sendSocket, daemon.MediaPort(), rtpPacket.data(), rtpPacket.size());
    const std::vector<uint8_t> srPacket = {
        0x80, 0xC8, 0x00, 0x06,
        0x11, 0x11, 0x11, 0x11,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x64,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x03,
    };
    (void)SendUdpPacket(sendSocket, daemon.MediaPort(), srPacket.data(), srPacket.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const std::vector<uint8_t> rrPacket = {
        0x81, 0xC9, 0x00, 0x07,
        0x22, 0x22, 0x22, 0x22,
        0x11, 0x11, 0x11, 0x11,
        0x40, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x01, 0xE0,
        0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    (void)SendUdpPacket(sendSocket, daemon.MediaPort(), rrPacket.data(), rrPacket.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    (void)SendUdpPacket(sendSocket, daemon.MediaPort(), rtpPacket.data(), rtpPacket.size());

    auto tryParseRemb = [&](const std::vector<uint8_t>& packet) -> bool {
        if (packet.size() < 24 ||
            packet[0] != 0x8F ||
            packet[1] != 0xCE ||
            packet[12] != 'R' ||
            packet[13] != 'E' ||
            packet[14] != 'M' ||
            packet[15] != 'B' ||
            packet[16] != 0x01) {
            return false;
        }

        const uint8_t exp = static_cast<uint8_t>((packet[17] >> 2U) & 0x3FU);
        const uint32_t mantissa = (static_cast<uint32_t>(packet[17] & 0x03U) << 16U) |
                                  (static_cast<uint32_t>(packet[18]) << 8U) |
                                  static_cast<uint32_t>(packet[19]);
        const uint64_t bitrate = static_cast<uint64_t>(mantissa) << exp;
        constexpr uint64_t kMaxUint32 = 0xFFFFFFFFULL;
        rembBitrateBps = bitrate > kMaxUint32
            ? 0xFFFFFFFFU
            : static_cast<uint32_t>(bitrate);
        rembTargetSsrc = (static_cast<uint32_t>(packet[20]) << 24U) |
                         (static_cast<uint32_t>(packet[21]) << 16U) |
                         (static_cast<uint32_t>(packet[22]) << 8U) |
                         static_cast<uint32_t>(packet[23]);
        return true;
    };

    for (int i = 0; i < 60 && (!receivedPositiveQuality.load() || !receivedRemb); ++i) {
        std::vector<uint8_t> incoming;
        if (!receivedRemb && ReceiveUdpPacket(sendSocket, &incoming, 50) && tryParseRemb(incoming)) {
            receivedRemb = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    CloseSocket(sendSocket);
    daemon.Stop();
    CloseSocket(listener);
    if (serverThread.joinable()) {
        serverThread.join();
    }
    CleanupSockets();

    const bool ok = receivedNode.load() &&
                    receivedPositiveQuality.load() &&
                    receivedRemb &&
                    qualityMeetingId == "room-quality" &&
                    qualityUserId == "alice" &&
                    qualityBitrateKbps > 0 &&
                    qualityPacketLoss > 0.24F &&
                    qualityPacketLoss < 0.26F &&
                    qualityRttMs > 0 &&
                    qualityJitterMs == 10U &&
                    rembTargetSsrc == 0x11111111U &&
                    rembBitrateBps > 0U;
    if (!ok) {
        std::cerr << "quality/remb check failed"
                  << " receivedNode=" << receivedNode.load()
                  << " receivedPositiveQuality=" << receivedPositiveQuality.load()
                  << " receivedRemb=" << receivedRemb
                  << " qualityMeetingId=" << qualityMeetingId
                  << " qualityUserId=" << qualityUserId
                  << " qualityBitrateKbps=" << qualityBitrateKbps
                  << " qualityPacketLoss=" << qualityPacketLoss
                  << " qualityRttMs=" << qualityRttMs
                  << " qualityJitterMs=" << qualityJitterMs
                  << " rembTargetSsrc=0x" << std::hex << rembTargetSsrc << std::dec
                  << " rembBitrateBps=" << rembBitrateBps
                  << "\n";
    }
    return ok;
}

} // namespace

int main() {
    const bool ok = TestLoadDaemonConfigFromEnv() &&
                    TestDaemonLifecycle() &&
                    TestDaemonReportsNodeStatusToSignaling() &&
                    TestDaemonReportsPublisherQualityToSignaling();
    if (!ok) {
        return 1;
    }

    std::cout << "sfu_daemon_tests passed\n";
    return 0;
}
