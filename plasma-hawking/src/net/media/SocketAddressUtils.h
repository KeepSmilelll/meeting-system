#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace media {

#ifdef _WIN32
enum class UdpSocketOpenError {
    None = 0,
    WsaStartupFailed,
    SocketCreateFailed,
    InvalidLocalAddress,
    BindFailed,
};

inline const char* udpSocketOpenErrorMessage(UdpSocketOpenError error) {
    switch (error) {
        case UdpSocketOpenError::None:
            return "";
        case UdpSocketOpenError::WsaStartupFailed:
            return "WSAStartup failed";
        case UdpSocketOpenError::SocketCreateFailed:
            return "socket() failed";
        case UdpSocketOpenError::InvalidLocalAddress:
            return "invalid local address";
        case UdpSocketOpenError::BindFailed:
            return "bind() failed";
    }
    return "socket setup failed";
}

inline UdpSocketOpenError openBoundUdpSocket(const std::string& localAddress,
                                             uint16_t localPort,
                                             SOCKET& outSocket) {
    static std::once_flag wsaInitOnce;
    static int wsaInitStatus = 0;
    std::call_once(wsaInitOnce, [] {
        WSADATA data{};
        wsaInitStatus = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (wsaInitStatus != 0) {
        return UdpSocketOpenError::WsaStartupFailed;
    }

    SOCKET socketHandle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        return UdpSocketOpenError::SocketCreateFailed;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(localPort);
    if (localAddress.empty() || localAddress == "0.0.0.0") {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (InetPtonA(AF_INET, localAddress.c_str(), &local.sin_addr) != 1) {
        ::closesocket(socketHandle);
        return UdpSocketOpenError::InvalidLocalAddress;
    }

    if (::bind(socketHandle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        ::closesocket(socketHandle);
        return UdpSocketOpenError::BindFailed;
    }

    outSocket = socketHandle;
    return UdpSocketOpenError::None;
}

inline void closeUdpSocket(SOCKET& socketHandle) {
    if (socketHandle != INVALID_SOCKET) {
        ::closesocket(socketHandle);
        socketHandle = INVALID_SOCKET;
    }
}

inline bool resolveIpv4PeerAddress(const std::string& address, uint16_t port, sockaddr_in& outPeer) {
    if (address.empty() || port == 0) {
        return false;
    }

    std::memset(&outPeer, 0, sizeof(outPeer));
    outPeer.sin_family = AF_INET;
    outPeer.sin_port = htons(port);
    return InetPtonA(AF_INET, address.c_str(), &outPeer.sin_addr) == 1;
}

inline bool sameIpv4Endpoint(const sockaddr_in& lhs, const sockaddr_in& rhs, bool matchPort = true) {
    if (lhs.sin_family != AF_INET || rhs.sin_family != AF_INET) {
        return false;
    }
    if (lhs.sin_addr.s_addr != rhs.sin_addr.s_addr) {
        return false;
    }
    return !matchPort || lhs.sin_port == rhs.sin_port;
}

inline bool looksLikeRtcpPacket(const uint8_t* data, std::size_t len) {
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

inline bool isTransientUdpRecvError() {
    const int errorCode = WSAGetLastError();
    return errorCode == WSAETIMEDOUT ||
           errorCode == WSAEINTR ||
           errorCode == WSAEWOULDBLOCK ||
           errorCode == WSAENOTSOCK;
}

inline uint16_t parseRtpSequenceNumber(const uint8_t* data, std::size_t len) {
    constexpr std::size_t kMinRtpHeaderBytes = 12U;
    if (data == nullptr || len < kMinRtpHeaderBytes) {
        return 0;
    }

    return static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) |
                                 static_cast<uint16_t>(data[3]));
}
#endif

}  // namespace media
