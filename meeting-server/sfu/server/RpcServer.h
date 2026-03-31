#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace sfu {

class RpcService;

class RpcServer final {
public:
    explicit RpcServer(uint16_t listenPort,
                       std::shared_ptr<RpcService> service = nullptr,
                       std::string advertisedHost = "127.0.0.1");
    ~RpcServer();

    bool Start();
    void Stop();

    bool Running() const noexcept;
    uint16_t Port() const noexcept;
    std::shared_ptr<RpcService> Service() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sfu
