#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sfu {

class NackBuffer final {
public:
    explicit NackBuffer(std::size_t capacity = 500, std::size_t maxPacketSize = 1600);

    void Store(uint16_t seq, const uint8_t* packet, std::size_t len);
    bool Get(uint16_t seq, std::vector<uint8_t>* out) const;
    bool Contains(uint16_t seq) const;

    std::size_t Size() const;
    std::size_t Capacity() const;

private:
    const std::size_t capacity_;
    const std::size_t maxPacketSize_;

    mutable std::mutex mu_;
    std::deque<uint16_t> order_;
    std::unordered_map<uint16_t, std::vector<uint8_t>> packets_;
};

} // namespace sfu
