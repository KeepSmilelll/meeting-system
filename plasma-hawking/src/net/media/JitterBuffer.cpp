#include "JitterBuffer.h"

#include <algorithm>
#include <thread>

namespace media {

JitterBuffer::JitterBuffer(std::size_t maxPackets,
                           std::size_t minBufferedPackets,
                           std::chrono::milliseconds gapTimeout)
    : m_maxPackets(maxPackets == 0 ? 1 : maxPackets),
      m_minBufferedPackets(std::max<std::size_t>(1, std::min<std::size_t>(minBufferedPackets, m_maxPackets))),
      m_gapTimeout(gapTimeout < std::chrono::milliseconds(0) ? std::chrono::milliseconds(0) : gapTimeout) {}

void JitterBuffer::setMaxPackets(std::size_t maxPackets) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxPackets = maxPackets == 0 ? 1 : maxPackets;
    m_minBufferedPackets = std::min<std::size_t>(m_minBufferedPackets, m_maxPackets);
    if (m_minBufferedPackets == 0) {
        m_minBufferedPackets = 1;
    }
    trimLocked();
}

std::size_t JitterBuffer::maxPackets() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxPackets;
}

void JitterBuffer::setMinBufferedPackets(std::size_t minBufferedPackets) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_minBufferedPackets = std::max<std::size_t>(1, std::min<std::size_t>(minBufferedPackets, m_maxPackets));
    m_cv.notify_one();
}

std::size_t JitterBuffer::minBufferedPackets() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_minBufferedPackets;
}

void JitterBuffer::setGapTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gapTimeout = timeout < std::chrono::milliseconds(0) ? std::chrono::milliseconds(0) : timeout;
    m_gapWaitStartedAt = {};
    m_cv.notify_one();
}

std::chrono::milliseconds JitterBuffer::gapTimeout() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_gapTimeout;
}

bool JitterBuffer::push(const RTPPacket& packet) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_packets[packet.header.sequenceNumber] = packet;
    if (!m_expectedSeq.has_value()) {
        m_expectedSeq = packet.header.sequenceNumber;
    }

    trimLocked();
    m_cv.notify_one();
    return true;
}

bool JitterBuffer::pop(RTPPacket& outPacket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return popLocked(outPacket);
}

bool JitterBuffer::popWait(RTPPacket& outPacket, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!readyToPopLocked(std::chrono::steady_clock::now())) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto waitSlice = m_gapTimeout.count() > 0
            ? std::min(remaining, m_gapTimeout)
            : remaining;
        if (waitSlice <= std::chrono::milliseconds(0)) {
            return false;
        }
        m_cv.wait_for(lock, waitSlice);
    }
    return popLocked(outPacket);
}

void JitterBuffer::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_packets.clear();
    m_expectedSeq.reset();
    m_gapWaitStartedAt = {};
    m_playoutStarted = false;
}

std::size_t JitterBuffer::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_packets.size();
}

bool JitterBuffer::popLocked(RTPPacket& outPacket) {
    const auto now = std::chrono::steady_clock::now();
    if (!readyToPopLocked(now)) {
        return false;
    }

    std::map<uint16_t, RTPPacket>::iterator it = m_packets.begin();
    if (m_expectedSeq.has_value()) {
        const auto expected = m_packets.find(*m_expectedSeq);
        if (expected != m_packets.end()) {
            it = expected;
        }
    }

    outPacket = std::move(it->second);
    const uint16_t consumedSeq = it->first;
    m_packets.erase(it);

    m_expectedSeq = static_cast<uint16_t>(consumedSeq + 1);
    m_gapWaitStartedAt = {};
    return true;
}

bool JitterBuffer::readyToPopLocked(std::chrono::steady_clock::time_point now) {
    if (m_packets.empty()) {
        return false;
    }

    if (!m_playoutStarted) {
        if (m_packets.size() < m_minBufferedPackets && m_packets.size() < m_maxPackets) {
            return false;
        }
        m_playoutStarted = true;
    }

    if (!m_expectedSeq.has_value() || m_packets.find(*m_expectedSeq) != m_packets.end()) {
        m_gapWaitStartedAt = {};
        return true;
    }

    if (m_packets.size() >= m_maxPackets || m_gapTimeout == std::chrono::milliseconds(0)) {
        return true;
    }

    if (m_gapWaitStartedAt == std::chrono::steady_clock::time_point{}) {
        m_gapWaitStartedAt = now;
        return false;
    }

    return now - m_gapWaitStartedAt >= m_gapTimeout;
}

void JitterBuffer::trimLocked() {
    while (m_packets.size() > m_maxPackets) {
        m_packets.erase(m_packets.begin());
    }
}

bool runJitterBufferSelfCheck() {
    JitterBuffer buffer(2);

    RTPPacket p1;
    p1.header.sequenceNumber = 100;
    p1.payload = {1};

    RTPPacket p2;
    p2.header.sequenceNumber = 101;
    p2.payload = {2};

    RTPPacket p3;
    p3.header.sequenceNumber = 102;
    p3.payload = {3};

    if (!buffer.push(p1) || !buffer.push(p2) || !buffer.push(p3)) {
        return false;
    }

    if (buffer.size() != 2) {
        return false;
    }

    RTPPacket out;
    if (!buffer.pop(out)) {
        return false;
    }

    if (out.header.sequenceNumber != 101) {
        return false;
    }

    JitterBuffer playoutBuffer(8, 3, std::chrono::milliseconds(20));
    if (!playoutBuffer.push(p1) || playoutBuffer.pop(out)) {
        return false;
    }
    if (!playoutBuffer.push(p3) || playoutBuffer.pop(out)) {
        return false;
    }
    if (!playoutBuffer.push(p2) || !playoutBuffer.pop(out) || out.header.sequenceNumber != 100) {
        return false;
    }
    if (!playoutBuffer.pop(out) || out.header.sequenceNumber != 101) {
        return false;
    }
    if (!playoutBuffer.pop(out) || out.header.sequenceNumber != 102) {
        return false;
    }

    JitterBuffer gapBuffer(8, 1, std::chrono::milliseconds(1));
    if (!gapBuffer.push(p1) || !gapBuffer.pop(out) || out.header.sequenceNumber != 100) {
        return false;
    }
    if (!gapBuffer.push(p3) || gapBuffer.pop(out)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return gapBuffer.pop(out) && out.header.sequenceNumber == 102;
}

}  // namespace media
