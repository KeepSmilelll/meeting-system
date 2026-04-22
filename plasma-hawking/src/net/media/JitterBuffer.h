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
    explicit JitterBuffer(std::size_t maxPackets = 128,
                          std::size_t minBufferedPackets = 1,
                          std::chrono::milliseconds gapTimeout = std::chrono::milliseconds(0));

    void setMaxPackets(std::size_t maxPackets);
    std::size_t maxPackets() const;
    void setMinBufferedPackets(std::size_t minBufferedPackets);
    std::size_t minBufferedPackets() const;
    void setGapTimeout(std::chrono::milliseconds timeout);
    std::chrono::milliseconds gapTimeout() const;

    bool push(const RTPPacket& packet);
    bool pop(RTPPacket& outPacket);
    bool popWait(RTPPacket& outPacket, std::chrono::milliseconds timeout);

    void clear();
    std::size_t size() const;

private:
    bool popLocked(RTPPacket& outPacket);
    bool readyToPopLocked(std::chrono::steady_clock::time_point now);
    void trimLocked();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<uint16_t, RTPPacket> m_packets;
    std::optional<uint16_t> m_expectedSeq;
    std::chrono::steady_clock::time_point m_gapWaitStartedAt{};
    std::size_t m_maxPackets;
    std::size_t m_minBufferedPackets;
    std::chrono::milliseconds m_gapTimeout;
    bool m_playoutStarted{false};
};

// Minimal runtime self-check for jitter buffering semantics.
bool runJitterBufferSelfCheck();

}  // namespace media
