#include "server/UdpServer.h"

#include <memory>
#include <utility>

#include <boost/system/error_code.hpp>

namespace sfu {

UdpServer::UdpServer(boost::asio::io_context& io, uint16_t listenPort, std::size_t maxPacketSize)
    : io_(io),
      socket_(io),
      recvBuffer_(maxPacketSize > 0 ? maxPacketSize : 2048),
      listenPort_(listenPort) {}

bool UdpServer::Start(PacketHandler handler) {
    if (running_.exchange(true)) {
        return true;
    }

    handler_ = std::move(handler);
    if (!handler_) {
        running_.store(false);
        return false;
    }

    boost::system::error_code ec;
    socket_.open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        running_.store(false);
        return false;
    }

    socket_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        boost::system::error_code closeEc;
        socket_.close(closeEc);
        running_.store(false);
        return false;
    }

    socket_.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), listenPort_), ec);
    if (ec) {
        boost::system::error_code closeEc;
        socket_.close(closeEc);
        running_.store(false);
        return false;
    }

    DoReceive();
    return true;
}

void UdpServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    boost::system::error_code ec;
    socket_.close(ec);
}

void UdpServer::SendTo(const uint8_t* data, std::size_t len, const Endpoint& to) {
    if (!running_.load() || data == nullptr || len == 0 || to.port() == 0) {
        return;
    }

    auto payload = std::make_shared<std::vector<uint8_t>>(data, data + len);
    boost::asio::post(io_, [this, payload, to]() {
        if (!running_.load()) {
            return;
        }

        socket_.async_send_to(
            boost::asio::buffer(*payload), to,
            [payload](const boost::system::error_code&, std::size_t) {
                // best-effort UDP forwarding; errors are intentionally ignored here
            });
    });
}

uint16_t UdpServer::Port() const {
    if (!socket_.is_open()) {
        return listenPort_;
    }

    boost::system::error_code ec;
    const auto ep = socket_.local_endpoint(ec);
    if (ec) {
        return listenPort_;
    }
    return ep.port();
}

void UdpServer::DoReceive() {
    if (!running_.load()) {
        return;
    }

    socket_.async_receive_from(
        boost::asio::buffer(recvBuffer_), remoteEndpoint_,
        [this](const boost::system::error_code& ec, std::size_t bytesReceived) {
            if (!running_.load()) {
                return;
            }

            if (!ec && bytesReceived > 0 && bytesReceived <= recvBuffer_.size() && handler_) {
                handler_(recvBuffer_.data(), bytesReceived, remoteEndpoint_);
            }

            if (running_.load()) {
                DoReceive();
            }
        });
}

} // namespace sfu
