#include "server/RpcServer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "server/RpcProtocol.h"
#include "server/RpcService.h"

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

} // namespace

struct RpcServer::Impl final {
    uint16_t listenPort{0};
    uint16_t actualPort{0};
    std::string advertisedHost{"127.0.0.1"};
    std::shared_ptr<RpcService> service;
    std::atomic<bool> running{false};
    std::thread acceptThread;
    SocketHandle listenSocket{kInvalidSocket};
    bool socketsStarted{false};
};

RpcServer::RpcServer(uint16_t listenPort, std::shared_ptr<RpcService> service, std::string advertisedHost)
    : impl_(std::make_unique<Impl>()) {
    impl_->listenPort = listenPort;
    impl_->service = service ? std::move(service) : std::make_shared<RpcService>();
    if (!advertisedHost.empty()) {
        impl_->advertisedHost = std::move(advertisedHost);
    }
}

RpcServer::~RpcServer() {
    Stop();
}

bool RpcServer::Start() {
    if (impl_ == nullptr) {
        return false;
    }
    if (impl_->running.exchange(true)) {
        return true;
    }

    if (!InitSockets()) {
        impl_->running.store(false);
        return false;
    }
    impl_->socketsStarted = true;

    impl_->listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listenSocket == kInvalidSocket) {
        Stop();
        return false;
    }

    int reuse = 1;
    (void)setsockopt(impl_->listenSocket, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), static_cast<int>(sizeof(reuse)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(impl_->listenPort);
    if (bind(impl_->listenSocket, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) != 0) {
        Stop();
        return false;
    }

    if (listen(impl_->listenSocket, SOMAXCONN) != 0) {
        Stop();
        return false;
    }

    sockaddr_in localAddr{};
#ifdef _WIN32
    int localAddrLen = static_cast<int>(sizeof(localAddr));
#else
    socklen_t localAddrLen = static_cast<socklen_t>(sizeof(localAddr));
#endif
    if (getsockname(impl_->listenSocket, reinterpret_cast<sockaddr*>(&localAddr), &localAddrLen) != 0) {
        Stop();
        return false;
    }
    impl_->actualPort = ntohs(localAddr.sin_port);
    impl_->service->SetAdvertisedAddress(impl_->advertisedHost + ":" + std::to_string(impl_->actualPort));

    impl_->acceptThread = std::thread([this]() {
        while (impl_->running.load()) {
            sockaddr_in clientAddr{};
#ifdef _WIN32
            int clientAddrLen = static_cast<int>(sizeof(clientAddr));
#else
            socklen_t clientAddrLen = static_cast<socklen_t>(sizeof(clientAddr));
#endif
            const auto client = accept(impl_->listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
            if (client == kInvalidSocket) {
                if (!impl_->running.load()) {
                    break;
                }
                continue;
            }

            std::vector<uint8_t> headerBytes(kRpcHeaderSize);
            RpcFrame requestFrame;
            bool ok = ReadExact(client, headerBytes.data(), headerBytes.size());
            if (ok) {
                const uint32_t payloadSize = (static_cast<uint32_t>(headerBytes[10]) << 24U) |
                                             (static_cast<uint32_t>(headerBytes[11]) << 16U) |
                                             (static_cast<uint32_t>(headerBytes[12]) << 8U) |
                                             static_cast<uint32_t>(headerBytes[13]);
                std::vector<uint8_t> requestBytes;
                requestBytes.reserve(headerBytes.size() + payloadSize);
                requestBytes.insert(requestBytes.end(), headerBytes.begin(), headerBytes.end());
                if (payloadSize > 0) {
                    std::vector<uint8_t> payload(payloadSize);
                    ok = ReadExact(client, payload.data(), payload.size());
                    requestBytes.insert(requestBytes.end(), payload.begin(), payload.end());
                }
                if (ok) {
                    ok = DecodeRpcFrame(requestBytes.data(), requestBytes.size(), &requestFrame) &&
                         requestFrame.kind == RpcFrameKind::kRequest;
                }
            }

            RpcFrame responseFrame;
            responseFrame.method = requestFrame.method;
            responseFrame.kind = RpcFrameKind::kResponse;
            responseFrame.status = 1;

            if (ok && impl_->service) {
                std::vector<uint8_t> responsePayload;
                if (impl_->service->Dispatch(requestFrame.method, requestFrame.payload.data(), requestFrame.payload.size(), &responsePayload)) {
                    responseFrame.status = 0;
                    responseFrame.payload = std::move(responsePayload);
                }
            }

            std::vector<uint8_t> responseBytes;
            if (EncodeRpcFrame(responseFrame, &responseBytes)) {
                (void)WriteExact(client, responseBytes.data(), responseBytes.size());
            }

            CloseSocket(client);
        }
    });

    return true;
}

void RpcServer::Stop() {
    if (impl_ == nullptr) {
        return;
    }

    const bool wasRunning = impl_->running.exchange(false);
    if (impl_->listenSocket != kInvalidSocket) {
#ifdef _WIN32
        shutdown(impl_->listenSocket, SD_BOTH);
#else
        shutdown(impl_->listenSocket, SHUT_RDWR);
#endif
        CloseSocket(impl_->listenSocket);
        impl_->listenSocket = kInvalidSocket;
    }

    if (impl_->acceptThread.joinable()) {
        impl_->acceptThread.join();
    }

    if (wasRunning || impl_->socketsStarted) {
        CleanupSockets();
        impl_->socketsStarted = false;
    }
}

bool RpcServer::Running() const noexcept {
    return impl_ != nullptr && impl_->running.load();
}

uint16_t RpcServer::Port() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    return impl_->actualPort != 0 ? impl_->actualPort : impl_->listenPort;
}

std::shared_ptr<RpcService> RpcServer::Service() const noexcept {
    return impl_ ? impl_->service : nullptr;
}

} // namespace sfu
