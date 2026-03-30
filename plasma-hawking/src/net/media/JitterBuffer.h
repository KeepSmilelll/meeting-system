#pragma once

#include "RTPReceiver.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace media {

class JitterBuffer {
public:
    explicit JitterBuffer(std::size_t maxPackets = 128);

    void setMaxPackets(std::size_t maxPackets);
    std::size_t maxPackets() const;

    bool push(const RTPPacket& packet);
    bool pop(RTPPacket& outPacket);
    bool popWait(RTPPacket& outPacket, std::chrono::milliseconds timeout);

    void clear();
    std::size_t size() const;

private:
    bool popLocked(RTPPacket& outPacket);
    void trimLocked();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<uint16_t, RTPPacket> m_packets;
    std::optional<uint16_t> m_expectedSeq;
    std::size_t m_maxPackets;
};

// Minimal runtime self-check for jitter buffering semantics.
bool runJitterBufferSelfCheck();

}  // namespace media
