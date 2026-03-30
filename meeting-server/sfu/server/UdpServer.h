#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <boost/asio.hpp>

namespace sfu {

class UdpServer final {
public:
    using Endpoint = boost::asio::ip::udp::endpoint;
    using PacketHandler = std::function<void(const uint8_t* data, std::size_t len, const Endpoint& from)>;

    UdpServer(boost::asio::io_context& io, uint16_t listenPort, std::size_t maxPacketSize = 2048);

    bool Start(PacketHandler handler);
    void Stop();

    void SendTo(const uint8_t* data, std::size_t len, const Endpoint& to);

    uint16_t Port() const;

private:
    void DoReceive();

    boost::asio::io_context& io_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint remoteEndpoint_;
    std::vector<uint8_t> recvBuffer_;
    PacketHandler handler_;
    std::atomic<bool> running_{false};
    uint16_t listenPort_{0};
};

} // namespace sfu
