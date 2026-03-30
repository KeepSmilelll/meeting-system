#include "rtp/RtpRouter.h"

#include <utility>

namespace sfu {

RtpRouter::RtpRouter(std::size_t nackCapacity)
    : nackCapacity_(nackCapacity > 0 ? nackCapacity : 500) {}

bool RtpRouter::RegisterPublisher(uint32_t sourceSsrc) {
    if (sourceSsrc == 0) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mu_);
    const auto [it, inserted] = publishers_.try_emplace(sourceSsrc, std::make_shared<PublisherRoute>(nackCapacity_));
    return inserted || it != publishers_.end();
}

void RtpRouter::UnregisterPublisher(uint32_t sourceSsrc) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    publishers_.erase(sourceSsrc);
}

bool RtpRouter::AddSubscriber(uint32_t sourceSsrc, uint32_t rewrittenSsrc, ForwardCallback forward) {
    if (sourceSsrc == 0 || rewrittenSsrc == 0 || !forward) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mu_);
    auto& route = publishers_[sourceSsrc];
    if (!route) {
        route = std::make_shared<PublisherRoute>(nackCapacity_);
    }

    route->subscribers.push_back(SubscriberRoute{rewrittenSsrc, std::move(forward)});
    return true;
}

void RtpRouter::RemoveSubscribers(uint32_t sourceSsrc) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    const auto it = publishers_.find(sourceSsrc);
    if (it == publishers_.end() || !it->second) {
        return;
    }

    it->second->subscribers.clear();
}

bool RtpRouter::Route(const uint8_t* packet, std::size_t len) {
    ParsedRtpPacket parsed;
    if (!parser_.Parse(packet, len, &parsed)) {
        return false;
    }

    std::shared_ptr<PublisherRoute> route;
    std::vector<SubscriberRoute> subscribers;
    {
        std::shared_lock<std::shared_mutex> lock(mu_);
        const auto it = publishers_.find(parsed.header.ssrc);
        if (it == publishers_.end() || !it->second) {
            return false;
        }

        route = it->second;
        subscribers = route->subscribers;
    }

    route->nack.Store(parsed.header.sequence, packet, len);

    for (const auto& sub : subscribers) {
        if (!sub.forward) {
            continue;
        }

        std::vector<uint8_t> out(packet, packet + len);
        RewriteSsrc(sub.rewrittenSsrc, out.data(), out.size());
        sub.forward(out.data(), out.size());
    }

    return true;
}

bool RtpRouter::GetRetransmitPacket(uint32_t sourceSsrc, uint16_t seq, std::vector<uint8_t>* out) const {
    if (out == nullptr) {
        return false;
    }

    std::shared_ptr<PublisherRoute> route;
    {
        std::shared_lock<std::shared_mutex> lock(mu_);
        const auto it = publishers_.find(sourceSsrc);
        if (it == publishers_.end() || !it->second) {
            return false;
        }

        route = it->second;
    }

    return route->nack.Get(seq, out);
}

void RtpRouter::RewriteSsrc(uint32_t ssrc, uint8_t* packet, std::size_t len) {
    if (packet == nullptr || len < RtpParser::kMinHeaderSize) {
        return;
    }

    packet[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
    packet[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    packet[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
    packet[11] = static_cast<uint8_t>(ssrc & 0xFF);
}

} // namespace sfu
