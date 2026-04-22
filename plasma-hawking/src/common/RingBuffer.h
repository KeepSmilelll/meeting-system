#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace common {

// Thread-safe bounded ring buffer with optional blocking pop.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity)
        : m_capacity(capacity == 0 ? 1 : capacity) {}

    bool push(const T& value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || m_queue.size() >= m_capacity) {
            return false;
        }
        m_queue.push_back(value);
        m_cv.notify_one();
        return true;
    }

    bool push(T&& value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || m_queue.size() >= m_capacity) {
            return false;
        }
        m_queue.push_back(std::move(value));
        m_cv.notify_one();
        return true;
    }

    bool pop(T& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    bool popWait(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, timeout, [this] { return m_closed || !m_queue.empty(); })) {
            return false;
        }
        if (m_queue.empty()) {
            return false;
        }
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        m_closed = false;
        m_cv.notify_all();
    }

    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_cv.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    std::size_t capacity() const {
        return m_capacity;
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

private:
    const std::size_t m_capacity;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<T> m_queue;
    bool m_closed{false};
};

}  // namespace common
