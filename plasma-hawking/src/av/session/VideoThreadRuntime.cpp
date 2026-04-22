#include "VideoThreadRuntime.h"

#include <QThread>

#include <utility>

namespace av::session {

VideoThreadRuntime::~VideoThreadRuntime() {
    joinCapture();
    joinSend();
    joinRecv();
}

bool VideoThreadRuntime::captureJoinable() const {
    return m_captureThread != nullptr;
}

bool VideoThreadRuntime::sendJoinable() const {
    return m_sendThread != nullptr;
}

bool VideoThreadRuntime::recvJoinable() const {
    return m_recvThread != nullptr;
}

void VideoThreadRuntime::startCapture(std::function<void()> entry, bool joinExisting) {
    if (joinExisting) {
        joinCapture();
    }
    if (m_captureThread != nullptr) {
        joinCapture();
    }
    m_captureThread = QThread::create([entry = std::move(entry)]() mutable {
        if (entry) {
            entry();
        }
    });
    if (m_captureThread != nullptr) {
        m_captureThread->start();
    }
}

void VideoThreadRuntime::startSend(std::function<void()> entry, bool joinExisting) {
    if (joinExisting) {
        joinSend();
    }
    if (m_sendThread != nullptr) {
        joinSend();
    }
    m_sendThread = QThread::create([entry = std::move(entry)]() mutable {
        if (entry) {
            entry();
        }
    });
    if (m_sendThread != nullptr) {
        m_sendThread->start();
    }
}

void VideoThreadRuntime::startRecv(std::function<void()> entry, bool joinExisting) {
    if (joinExisting) {
        joinRecv();
    }
    if (m_recvThread != nullptr) {
        joinRecv();
    }
    m_recvThread = QThread::create([entry = std::move(entry)]() mutable {
        if (entry) {
            entry();
        }
    });
    if (m_recvThread != nullptr) {
        m_recvThread->start();
    }
}

void VideoThreadRuntime::joinCapture() {
    if (m_captureThread == nullptr) {
        return;
    }
    m_captureThread->wait();
    delete m_captureThread;
    m_captureThread = nullptr;
}

void VideoThreadRuntime::joinSend() {
    if (m_sendThread == nullptr) {
        return;
    }
    m_sendThread->wait();
    delete m_sendThread;
    m_sendThread = nullptr;
}

void VideoThreadRuntime::joinRecv() {
    if (m_recvThread == nullptr) {
        return;
    }
    m_recvThread->wait();
    delete m_recvThread;
    m_recvThread = nullptr;
}

}  // namespace av::session
