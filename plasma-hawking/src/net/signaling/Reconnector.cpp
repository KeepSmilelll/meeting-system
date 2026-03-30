#include "Reconnector.h"

#include <algorithm>

namespace signaling {

Reconnector::Reconnector(QObject* parent)
    : QObject(parent) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        emit activeChanged(false);
        emit reconnectRequested();
    });
}

void Reconnector::configure(int initialDelayMs, int maxDelayMs) {
    m_initialDelayMs = std::max(100, initialDelayMs);
    m_maxDelayMs = std::max(m_initialDelayMs, maxDelayMs);
    m_nextDelayMs = m_initialDelayMs;
}

void Reconnector::schedule() {
    if (m_timer.isActive()) {
        return;
    }

    m_timer.start(m_nextDelayMs);
    emit activeChanged(true);
    m_nextDelayMs = std::min(m_nextDelayMs * 2, m_maxDelayMs);
}

void Reconnector::reset() {
    stop();
    m_nextDelayMs = m_initialDelayMs;
}

void Reconnector::stop() {
    if (m_timer.isActive()) {
        m_timer.stop();
        emit activeChanged(false);
    }
}

bool Reconnector::active() const {
    return m_timer.isActive();
}

}  // namespace signaling
