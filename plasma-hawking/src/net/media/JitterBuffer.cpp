#include "JitterBuffer.h"

namespace media {

JitterBuffer::JitterBuffer(std::size_t maxPackets)
    : m_maxPackets(maxPackets == 0 ? 1 : maxPackets) {}

void JitterBuffer::setMaxPackets(std::size_t maxPackets) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxPackets = maxPackets == 0 ? 1 : maxPackets;
    trimLocked();
}

std::size_t JitterBuffer::maxPackets() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxPackets;
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
    if (!m_cv.wait_for(lock, timeout, [this] { return !m_packets.empty(); })) {
        return false;
    }
    return popLocked(outPacket);
}

void JitterBuffer::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_packets.clear();
    m_expectedSeq.reset();
}

std::size_t JitterBuffer::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_packets.size();
}

bool JitterBuffer::popLocked(RTPPacket& outPacket) {
    if (m_packets.empty()) {
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
    return true;
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

    return out.header.sequenceNumber == 101;
}

}  // namespace media
