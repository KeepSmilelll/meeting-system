#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace net {

struct NetworkEndpoint {
    std::string address;
    uint16_t port{0};
};

struct NetworkPacket {
    std::vector<uint8_t> payload;
    NetworkEndpoint endpoint;
};

class INetworkTransport {
public:
    virtual ~INetworkTransport() = default;

    virtual bool open(const NetworkEndpoint& localEndpoint, std::string* error = nullptr) = 0;
    virtual bool sendTo(const NetworkEndpoint& endpoint,
                        const uint8_t* data,
                        std::size_t size,
                        std::string* error = nullptr) = 0;
    virtual bool receive(NetworkPacket& packet,
                         std::chrono::milliseconds timeout,
                         std::string* error = nullptr) = 0;
    virtual void close() = 0;
};

}  // namespace net
