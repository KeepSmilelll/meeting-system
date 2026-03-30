#include "rtp/NackBuffer.h"

namespace sfu {

NackBuffer::NackBuffer(std::size_t capacity, std::size_t maxPacketSize)
    : capacity_(capacity > 0 ? capacity : 500),
      maxPacketSize_(maxPacketSize > 0 ? maxPacketSize : 1600) {}

void NackBuffer::Store(uint16_t seq, const uint8_t* packet, std::size_t len) {
    if (packet == nullptr || len == 0 || len > maxPacketSize_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mu_);

    auto it = packets_.find(seq);
    if (it != packets_.end()) {
        it->second.assign(packet, packet + len);
        return;
    }

    if (packets_.size() >= capacity_ && !order_.empty()) {
        const uint16_t oldest = order_.front();
        order_.pop_front();
        packets_.erase(oldest);
    }

    order_.push_back(seq);
    packets_.emplace(seq, std::vector<uint8_t>(packet, packet + len));
}

bool NackBuffer::Get(uint16_t seq, std::vector<uint8_t>* out) const {
    if (out == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    const auto it = packets_.find(seq);
    if (it == packets_.end()) {
        return false;
    }

    *out = it->second;
    return true;
}

bool NackBuffer::Contains(uint16_t seq) const {
    std::lock_guard<std::mutex> lock(mu_);
    return packets_.find(seq) != packets_.end();
}

std::size_t NackBuffer::Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return packets_.size();
}

std::size_t NackBuffer::Capacity() const {
    return capacity_;
}

} // namespace sfu
