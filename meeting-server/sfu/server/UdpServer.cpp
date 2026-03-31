#include "server/UdpServer.h"

#include <utility>

namespace sfu {

UdpServer::UdpServer(std::size_t maxPacketSize, uint16_t listenPort)
    : recvBuffer_(maxPacketSize > 0 ? maxPacketSize : 2048)
    , listenPort_(listenPort) {}

UdpServer::~UdpServer() {
    Stop();
}

bool UdpServer::InitSockets() {
#ifdef _WIN32
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void UdpServer::CleanupSockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void UdpServer::CloseSocket(SocketHandle socketHandle) {
    if (socketHandle == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}

bool UdpServer::SetReuseAddress(SocketHandle socketHandle) {
    int reuse = 1;
    return setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&reuse), static_cast<int>(sizeof(reuse))) == 0;
}

bool UdpServer::BindAny(SocketHandle socketHandle, uint16_t listenPort) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(listenPort);
    return bind(socketHandle, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) == 0;
}

bool UdpServer::GetLocalPort(SocketHandle socketHandle, uint16_t* outPort) {
    if (outPort == nullptr) {
        return false;
    }

    sockaddr_in addr{};
#ifdef _WIN32
    int addrLen = static_cast<int>(sizeof(addr));
#else
    socklen_t addrLen = static_cast<socklen_t>(sizeof(addr));
#endif
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        return false;
    }

    *outPort = ntohs(addr.sin_port);
    return true;
}

bool UdpServer::Start(PacketHandler handler) {
    if (running_.exchange(true)) {
        return true;
    }

    handler_ = std::move(handler);
    if (!handler_) {
        running_.store(false);
        return false;
    }

    if (!InitSockets()) {
        running_.store(false);
        return false;
    }
    socketsStarted_ = true;

    socket_ = static_cast<SocketHandle>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (socket_ == kInvalidSocket) {
        Stop();
        return false;
    }

    if (!SetReuseAddress(socket_) || !BindAny(socket_, listenPort_)) {
        Stop();
        return false;
    }

    if (!GetLocalPort(socket_, &listenPort_)) {
        Stop();
        return false;
    }

    receiverThread_ = std::thread([this]() {
        ReceiveLoop();
    });

    return true;
}

void UdpServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    CloseSocket(socket_);
    socket_ = kInvalidSocket;

    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }

    if (socketsStarted_) {
        CleanupSockets();
        socketsStarted_ = false;
    }
}

void UdpServer::SendTo(const uint8_t* data, std::size_t len, const Endpoint& to) {
    if (!running_.load() || data == nullptr || len == 0 || to.sin_port == 0 || socket_ == kInvalidSocket) {
        return;
    }

    std::lock_guard<std::mutex> lock(sendMutex_);
    const auto sent = sendto(socket_,
                             reinterpret_cast<const char*>(data),
                             static_cast<int>(len),
                             0,
                             reinterpret_cast<const sockaddr*>(&to),
                             static_cast<int>(sizeof(to)));
    (void)sent;
}

uint16_t UdpServer::Port() const {
    if (socket_ == kInvalidSocket) {
        return listenPort_;
    }

    uint16_t port = listenPort_;
    if (GetLocalPort(socket_, &port)) {
        return port;
    }
    return listenPort_;
}

void UdpServer::ReceiveLoop() {
    while (running_.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket_, &readSet);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;

#ifdef _WIN32
        const int rc = select(0, &readSet, nullptr, nullptr, &tv);
#else
        const int rc = select(socket_ + 1, &readSet, nullptr, nullptr, &tv);
#endif
        if (rc <= 0 || !FD_ISSET(socket_, &readSet)) {
            continue;
        }

        Endpoint from{};
#ifdef _WIN32
        int fromLen = static_cast<int>(sizeof(from));
#else
        socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
#endif
        const auto received = recvfrom(socket_,
                                       reinterpret_cast<char*>(recvBuffer_.data()),
                                       static_cast<int>(recvBuffer_.size()),
                                       0,
                                       reinterpret_cast<sockaddr*>(&from),
                                       &fromLen);
        if (!running_.load()) {
            break;
        }
        if (received <= 0) {
            continue;
        }

        if (handler_) {
            handler_(recvBuffer_.data(), static_cast<std::size_t>(received), from);
        }
    }
}

} // namespace sfu
