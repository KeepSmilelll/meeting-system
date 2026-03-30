#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "rtp/NackBuffer.h"
#include "rtp/RtpParser.h"

namespace sfu {

class RtpRouter final {
public:
    using ForwardCallback = std::function<void(const uint8_t* data, std::size_t len)>;

    explicit RtpRouter(std::size_t nackCapacity = 500);

    bool RegisterPublisher(uint32_t sourceSsrc);
    void UnregisterPublisher(uint32_t sourceSsrc);

    bool AddSubscriber(uint32_t sourceSsrc, uint32_t rewrittenSsrc, ForwardCallback forward);
    void RemoveSubscribers(uint32_t sourceSsrc);

    bool Route(const uint8_t* packet, std::size_t len);
    bool GetRetransmitPacket(uint32_t sourceSsrc, uint16_t seq, std::vector<uint8_t>* out) const;

private:
    struct SubscriberRoute {
        uint32_t rewrittenSsrc{0};
        ForwardCallback forward;
    };

    struct PublisherRoute {
        explicit PublisherRoute(std::size_t nackCap) : nack(nackCap) {}

        NackBuffer nack;
        std::vector<SubscriberRoute> subscribers;
    };

    static void RewriteSsrc(uint32_t ssrc, uint8_t* packet, std::size_t len);

    const std::size_t nackCapacity_;
    mutable std::shared_mutex mu_;
    std::unordered_map<uint32_t, std::shared_ptr<PublisherRoute>> publishers_;
    RtpParser parser_;
};

} // namespace sfu
