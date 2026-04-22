#pragma once

#include <functional>

class QThread;

namespace av::session {

class VideoThreadRuntime {
public:
    ~VideoThreadRuntime();

    bool captureJoinable() const;
    bool sendJoinable() const;
    bool recvJoinable() const;

    void startCapture(std::function<void()> entry, bool joinExisting);
    void startSend(std::function<void()> entry, bool joinExisting);
    void startRecv(std::function<void()> entry, bool joinExisting);

    void joinCapture();
    void joinSend();
    void joinRecv();

private:
    QThread* m_captureThread{nullptr};
    QThread* m_sendThread{nullptr};
    QThread* m_recvThread{nullptr};
};

}  // namespace av::session
