#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
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

namespace sfu {

class UdpServer final {
public:
#ifdef _WIN32
    using SocketHandle = SOCKET;
    static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
    using SocketHandle = int;
    static constexpr SocketHandle kInvalidSocket = -1;
#endif

    using Endpoint = sockaddr_in;
    using PacketHandler = std::function<void(const uint8_t* data, std::size_t len, const Endpoint& from)>;

    UdpServer(std::size_t maxPacketSize = 2048, uint16_t listenPort = 0);
    ~UdpServer();

    bool Start(PacketHandler handler);
    void Stop();

    void SendTo(const uint8_t* data, std::size_t len, const Endpoint& to);

    uint16_t Port() const;

private:
    void ReceiveLoop();
    static bool InitSockets();
    static void CleanupSockets();
    static void CloseSocket(SocketHandle socketHandle);
    static bool SetReuseAddress(SocketHandle socketHandle);
    static bool BindAny(SocketHandle socketHandle, uint16_t listenPort);
    static bool GetLocalPort(SocketHandle socketHandle, uint16_t* outPort);

    SocketHandle socket_{kInvalidSocket};
    std::vector<uint8_t> recvBuffer_;
    PacketHandler handler_;
    std::thread receiverThread_;
    std::atomic<bool> running_{false};
    uint16_t listenPort_{0};
    bool socketsStarted_{false};
    mutable std::mutex sendMutex_;
};

} // namespace sfu
