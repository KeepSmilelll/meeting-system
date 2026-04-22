#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <type_traits>

#include <QByteArray>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QThread>
#include <QWaitCondition>
#include <QVariant>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QUdpSocket>

namespace media {

struct UdpEndpoint {
    QHostAddress address{};
    uint16_t port{0};

    bool isValid() const {
        return !address.isNull() && port != 0;
    }
};

class UdpPeerSocket {
public:
    ~UdpPeerSocket() {
        close();
    }

    bool open(const std::string& localAddress, uint16_t localPort, std::string* errorMessage = nullptr) {
        close();

        QHostAddress bindAddress = QHostAddress::AnyIPv4;
        if (!localAddress.empty() && localAddress != "0.0.0.0") {
            const QString bindAddressText = QString::fromStdString(localAddress);
            QHostAddress parsed;
            if (!parsed.setAddress(bindAddressText) || parsed.protocol() != QAbstractSocket::IPv4Protocol) {
                setSocketError(QAbstractSocket::SocketAddressNotAvailableError, "invalid local address");
                if (errorMessage != nullptr) {
                    *errorMessage = m_lastErrorString;
                }
                return false;
            }
            bindAddress = parsed;
        }

        auto* ioThread = new QThread();
        auto* ioContext = new QObject();
        ioContext->moveToThread(ioThread);
        ioThread->start();
        {
            QMutexLocker locker(&m_stateMutex);
            m_ioThread = ioThread;
            m_ioContext = ioContext;
            m_socket = nullptr;
            m_useSocketThread = true;
            m_peer = {};
            m_peerValid = false;
        }
        {
            QMutexLocker locker(&m_datagramMutex);
            m_pendingDatagrams.clear();
            m_waitInterrupted = false;
        }

        QAbstractSocket::SocketError bindError = QAbstractSocket::UnknownSocketError;
        std::string bindErrorString;
        const bool bound = invokeOnSocketThread(
            [this, ioContext, bindAddress, localPort, &bindError, &bindErrorString]() -> bool {
                auto* socket = new QUdpSocket(ioContext);
                if (!socket->bind(bindAddress,
                                  localPort,
                                  QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
                    bindError = socket->error();
                    bindErrorString = socket->errorString().toStdString();
                    delete socket;
                    return false;
                }
                m_socket = socket;
                QObject::connect(
                    socket,
                    &QUdpSocket::readyRead,
                    socket,
                    [this]() { enqueuePendingDatagramsFromSocket(); },
                    Qt::DirectConnection);
                return true;
            });
        if (!bound) {
            setSocketError(bindError, bindErrorString);
            if (errorMessage != nullptr) {
                *errorMessage = m_lastErrorString;
            }
            close();
            return false;
        }

        clearSocketError();
        setReadTimeoutMs(m_readTimeoutMs);
        return true;
    }

    void close() {
        interruptWaiters();

        QThread* ioThread = nullptr;
        QObject* ioContext = nullptr;
        QUdpSocket* socket = nullptr;
        bool useSocketThread = false;
        {
            QMutexLocker locker(&m_stateMutex);
            ioThread = m_ioThread;
            ioContext = m_ioContext;
            socket = m_socket;
            useSocketThread = m_useSocketThread;
        }

        if (!useSocketThread) {
            if (socket != nullptr) {
                socket->close();
                delete socket;
            }
            QMutexLocker locker(&m_stateMutex);
            m_socket = nullptr;
            m_ioContext = nullptr;
            m_ioThread = nullptr;
            m_useSocketThread = false;
            m_peer = {};
            m_peerValid = false;
            m_lastSocketError = QAbstractSocket::UnknownSocketError;
            m_lastErrorString.clear();
            {
                QMutexLocker queueLocker(&m_datagramMutex);
                m_pendingDatagrams.clear();
                m_waitInterrupted = false;
            }
            return;
        }

        if (ioContext != nullptr) {
            const auto closeSocket = [this]() {
                if (m_socket != nullptr) {
                    m_socket->close();
                    delete m_socket;
                    m_socket = nullptr;
                }
            };
            if (QThread::currentThread() == ioContext->thread()) {
                closeSocket();
                ioContext->deleteLater();
            } else {
                (void)QMetaObject::invokeMethod(ioContext, closeSocket, Qt::BlockingQueuedConnection);
                (void)QMetaObject::invokeMethod(ioContext, "deleteLater", Qt::QueuedConnection);
            }
        }

        if (ioThread != nullptr) {
            ioThread->quit();
            if (QThread::currentThread() != ioThread) {
                ioThread->wait();
                delete ioThread;
            }
        }

        QMutexLocker locker(&m_stateMutex);
        m_socket = nullptr;
        m_ioContext = nullptr;
        m_ioThread = nullptr;
        m_useSocketThread = false;
        m_peer = {};
        m_peerValid = false;
        m_lastSocketError = QAbstractSocket::UnknownSocketError;
        m_lastErrorString.clear();
        {
            QMutexLocker queueLocker(&m_datagramMutex);
            m_pendingDatagrams.clear();
            m_waitInterrupted = false;
        }
    }

    bool isOpen() const {
        QMutexLocker locker(&m_stateMutex);
        return m_socket != nullptr;
    }

    bool setPeer(const std::string& address, uint16_t port) {
        UdpEndpoint endpoint{};
        if (!resolveIpv4PeerAddress(address, port, endpoint)) {
            QMutexLocker locker(&m_stateMutex);
            m_peerValid = false;
            return false;
        }
        QMutexLocker locker(&m_stateMutex);
        m_peer = endpoint;
        m_peerValid = true;
        return true;
    }

    bool hasPeer() const {
        QMutexLocker locker(&m_stateMutex);
        return m_peerValid;
    }

    const UdpEndpoint& peer() const {
        QMutexLocker locker(&m_stateMutex);
        return m_peer;
    }

    uint16_t localPort() const {
        const int port = invokeOnSocketThread([this]() -> int {
            if (m_socket == nullptr) {
                return 0;
            }
            return static_cast<int>(m_socket->localPort());
        });
        return static_cast<uint16_t>(port);
    }

    bool acceptSender(const UdpEndpoint& from) const {
        UdpEndpoint peerSnapshot{};
        bool peerValidSnapshot = false;
        {
            QMutexLocker locker(&m_stateMutex);
            peerSnapshot = m_peer;
            peerValidSnapshot = m_peerValid;
        }
        if (!peerValidSnapshot) {
            return true;
        }
        return sameIpv4Endpoint(from, peerSnapshot, true);
    }

    bool acceptRtcpFromPeerHost(const UdpEndpoint& from) const {
        UdpEndpoint peerSnapshot{};
        bool peerValidSnapshot = false;
        {
            QMutexLocker locker(&m_stateMutex);
            peerSnapshot = m_peer;
            peerValidSnapshot = m_peerValid;
        }
        if (!peerValidSnapshot) {
            return true;
        }
        return sameIpv4Endpoint(from, peerSnapshot, false);
    }

    bool configureSocketBuffers(int recvBufferBytes,
                                int sendBufferBytes,
                                std::string* errorMessage = nullptr) {
        const bool configured = invokeOnSocketThread(
            [this, recvBufferBytes, sendBufferBytes]() -> bool {
                if (m_socket == nullptr) {
                    setSocketError(QAbstractSocket::OperationError, "socket not open");
                    return false;
                }
                m_socket->setSocketOption(
                    QAbstractSocket::ReceiveBufferSizeSocketOption, QVariant(recvBufferBytes));
                m_socket->setSocketOption(
                    QAbstractSocket::SendBufferSizeSocketOption, QVariant(sendBufferBytes));
                clearSocketError();
                return true;
            });
        if (!configured && errorMessage != nullptr) {
            *errorMessage = lastErrorString();
        }
        return configured;
    }

    void setReadTimeoutMs(int timeoutMs) {
        QMutexLocker locker(&m_stateMutex);
        m_readTimeoutMs = (std::max)(0, timeoutMs);
    }

    void interruptWaiters() {
        {
            QMutexLocker locker(&m_datagramMutex);
            m_waitInterrupted = true;
        }
        m_datagramReadyCondition.wakeAll();
    }

    int sendToPeer(const uint8_t* data, std::size_t len) const {
        UdpEndpoint peerSnapshot{};
        {
            QMutexLocker locker(&m_stateMutex);
            if (!m_peerValid) {
                return -1;
            }
            peerSnapshot = m_peer;
        }
        return sendTo(data, len, peerSnapshot);
    }

    int sendTo(const uint8_t* data, std::size_t len, const UdpEndpoint& target) const {
        if (data == nullptr || len == 0 || !target.isValid()) {
            return -1;
        }

        return invokeOnSocketThread([this, data, len, target]() -> int {
            if (m_socket == nullptr) {
                setSocketError(QAbstractSocket::OperationError, "socket not open");
                return -1;
            }
            const qint64 sent = m_socket->writeDatagram(
                reinterpret_cast<const char*>(data),
                static_cast<qint64>(len),
                target.address,
                target.port);
            if (sent < 0) {
                captureSocketError();
                return -1;
            }
            clearSocketError();
            return static_cast<int>(sent);
        });
    }

    int recvFrom(uint8_t* data, std::size_t len, UdpEndpoint& from) const {
        if (data == nullptr || len == 0) {
            return -1;
        }

        if (usingSocketThread()) {
            PendingDatagram pending{};
            bool hasPendingDatagram = false;
            {
                QMutexLocker locker(&m_datagramMutex);
                if (!m_pendingDatagrams.empty()) {
                    pending = std::move(m_pendingDatagrams.front());
                    m_pendingDatagrams.pop_front();
                    hasPendingDatagram = true;
                }
            }

            if (!hasPendingDatagram) {
                setSocketError(QAbstractSocket::TemporaryError, "no pending datagram");
                return -1;
            }

            const std::size_t copied =
                (std::min)(len, static_cast<std::size_t>(pending.payload.size()));
            std::memcpy(data, pending.payload.constData(), copied);
            from = pending.from;
            clearSocketError();
            return static_cast<int>(copied);
        }

        return invokeOnSocketThread([this, data, len, &from]() -> int {
            if (m_socket == nullptr) {
                setSocketError(QAbstractSocket::OperationError, "socket not open");
                return -1;
            }
            if (!m_socket->hasPendingDatagrams()) {
                setSocketError(QAbstractSocket::TemporaryError, "no pending datagram");
                return -1;
            }

            QNetworkDatagram datagram = m_socket->receiveDatagram(static_cast<qint64>(len));
            if (!datagram.isValid()) {
                captureSocketError();
                return -1;
            }

            const QByteArray payload = datagram.data();
            const std::size_t copied = (std::min)(len, static_cast<std::size_t>(payload.size()));
            std::memcpy(data, payload.constData(), copied);
            from.address = datagram.senderAddress();
            from.port = static_cast<uint16_t>(datagram.senderPort());
            clearSocketError();
            return static_cast<int>(copied);
        });
    }

    int waitForReadable(int timeoutMs) const {
        if (usingSocketThread()) {
            const int effectiveTimeoutMs = timeoutMs >= 0 ? timeoutMs : -1;
            enum class WaitOutcome {
                Ready,
                Interrupted,
                Timeout,
                SpuriousWake,
            };
            WaitOutcome outcome = WaitOutcome::SpuriousWake;

            {
                QMutexLocker locker(&m_datagramMutex);
                if (!m_pendingDatagrams.empty()) {
                    outcome = WaitOutcome::Ready;
                } else if (m_waitInterrupted) {
                    m_waitInterrupted = false;
                    outcome = WaitOutcome::Interrupted;
                } else {
                    bool woke = false;
                    if (effectiveTimeoutMs < 0) {
                        m_datagramReadyCondition.wait(&m_datagramMutex);
                        woke = true;
                    } else {
                        woke = m_datagramReadyCondition.wait(&m_datagramMutex, effectiveTimeoutMs);
                    }

                    if (m_waitInterrupted) {
                        m_waitInterrupted = false;
                        outcome = WaitOutcome::Interrupted;
                    } else if (!m_pendingDatagrams.empty()) {
                        outcome = WaitOutcome::Ready;
                    } else if (!woke) {
                        outcome = WaitOutcome::Timeout;
                    } else {
                        outcome = WaitOutcome::SpuriousWake;
                    }
                }
            }

            if (outcome == WaitOutcome::Ready) {
                clearSocketError();
                return 1;
            }
            if (outcome == WaitOutcome::Interrupted) {
                clearSocketError();
                return 0;
            }
            if (outcome == WaitOutcome::Timeout) {
                setSocketError(QAbstractSocket::SocketTimeoutError, "wait timeout");
                return 0;
            }
            setSocketError(QAbstractSocket::TemporaryError, "spurious wake without datagram");
            return 0;
        }

        const int effectiveTimeoutMs = timeoutMs >= 0 ? timeoutMs : -1;
        return invokeOnSocketThread([this, effectiveTimeoutMs]() -> int {
            if (m_socket == nullptr) {
                setSocketError(QAbstractSocket::OperationError, "socket not open");
                return -1;
            }
            if (m_socket->hasPendingDatagrams()) {
                clearSocketError();
                return 1;
            }
            if (m_socket->waitForReadyRead(effectiveTimeoutMs)) {
                clearSocketError();
                return 1;
            }
            captureSocketError();
            return lastSocketError() == QAbstractSocket::SocketTimeoutError ? 0 : -1;
        });
    }

    QAbstractSocket::SocketError lastSocketError() const {
        QMutexLocker locker(&m_stateMutex);
        return m_lastSocketError;
    }

    const std::string& lastErrorString() const {
        QMutexLocker locker(&m_stateMutex);
        return m_lastErrorString;
    }

    bool isTransientSocketError() const {
        QMutexLocker locker(&m_stateMutex);
        return m_lastSocketError == QAbstractSocket::SocketTimeoutError ||
               m_lastSocketError == QAbstractSocket::TemporaryError ||
               m_lastSocketError == QAbstractSocket::OperationError;
    }

    static bool sameIpv4Endpoint(const UdpEndpoint& lhs, const UdpEndpoint& rhs, bool matchPort = true) {
        if (!isIpv4(lhs) || !isIpv4(rhs)) {
            return false;
        }
        if (lhs.address.toIPv4Address() != rhs.address.toIPv4Address()) {
            return false;
        }
        return !matchPort || lhs.port == rhs.port;
    }

private:
    static bool isIpv4(const UdpEndpoint& endpoint) {
        return endpoint.port != 0 && endpoint.address.protocol() == QAbstractSocket::IPv4Protocol;
    }

    static bool resolveIpv4PeerAddress(const std::string& address, uint16_t port, UdpEndpoint& outPeer) {
        if (address.empty() || port == 0) {
            return false;
        }
        QHostAddress parsed;
        if (!parsed.setAddress(QString::fromStdString(address)) ||
            parsed.protocol() != QAbstractSocket::IPv4Protocol) {
            return false;
        }
        outPeer.address = parsed;
        outPeer.port = port;
        return true;
    }

    bool usingSocketThread() const {
        QMutexLocker locker(&m_stateMutex);
        return m_useSocketThread;
    }

    struct PendingDatagram {
        QByteArray payload{};
        UdpEndpoint from{};
    };

    void enqueuePendingDatagramsFromSocket() {
        if (m_socket == nullptr) {
            return;
        }

        std::deque<PendingDatagram> accepted;
        while (m_socket->hasPendingDatagrams()) {
            QNetworkDatagram datagram = m_socket->receiveDatagram();
            if (!datagram.isValid()) {
                captureSocketError();
                continue;
            }

            PendingDatagram pending{};
            pending.payload = datagram.data();
            pending.from.address = datagram.senderAddress();
            pending.from.port = static_cast<uint16_t>(datagram.senderPort());
            accepted.push_back(std::move(pending));
        }

        if (accepted.empty()) {
            return;
        }

        {
            QMutexLocker locker(&m_datagramMutex);
            for (auto& pending : accepted) {
                m_pendingDatagrams.push_back(std::move(pending));
            }
        }
        m_datagramReadyCondition.wakeAll();
        clearSocketError();
    }

    int readTimeoutMs() const {
        QMutexLocker locker(&m_stateMutex);
        return m_readTimeoutMs;
    }

    template <typename Fn>
    auto invokeOnSocketThread(Fn&& fn) const -> decltype(fn()) {
        using ReturnType = decltype(fn());

        bool useSocketThread = false;
        QObject* ioContext = nullptr;
        {
            QMutexLocker locker(&m_stateMutex);
            useSocketThread = m_useSocketThread;
            ioContext = m_ioContext;
        }
        if (!useSocketThread) {
            return fn();
        }
        if (ioContext == nullptr) {
            if constexpr (std::is_void_v<ReturnType>) {
                return;
            } else {
                return ReturnType{};
            }
        }

        if (QThread::currentThread() == ioContext->thread()) {
            return fn();
        }

        if constexpr (std::is_void_v<ReturnType>) {
            (void)QMetaObject::invokeMethod(ioContext, std::forward<Fn>(fn), Qt::BlockingQueuedConnection);
            return;
        } else {
            std::optional<ReturnType> result;
            (void)QMetaObject::invokeMethod(
                ioContext,
                [callable = std::forward<Fn>(fn), &result]() mutable { result = callable(); },
                Qt::BlockingQueuedConnection);
            return result.has_value() ? std::move(result.value()) : ReturnType{};
        }
    }

    void setSocketError(QAbstractSocket::SocketError error, std::string message) const {
        QMutexLocker locker(&m_stateMutex);
        m_lastSocketError = error;
        m_lastErrorString = std::move(message);
    }

    void captureSocketError() const {
        QMutexLocker locker(&m_stateMutex);
        if (m_socket != nullptr) {
            m_lastSocketError = m_socket->error();
            m_lastErrorString = m_socket->errorString().toStdString();
        } else {
            m_lastSocketError = QAbstractSocket::UnknownSocketError;
            m_lastErrorString = "socket not open";
        }
    }

    void clearSocketError() const {
        QMutexLocker locker(&m_stateMutex);
        m_lastSocketError = QAbstractSocket::UnknownSocketError;
        m_lastErrorString.clear();
    }

    mutable QMutex m_stateMutex;
    QThread* m_ioThread{nullptr};
    QObject* m_ioContext{nullptr};
    QUdpSocket* m_socket{nullptr};
    bool m_useSocketThread{false};
    int m_readTimeoutMs{50};
    UdpEndpoint m_peer{};
    bool m_peerValid{false};
    mutable QMutex m_datagramMutex;
    mutable QWaitCondition m_datagramReadyCondition;
    mutable std::deque<PendingDatagram> m_pendingDatagrams;
    mutable bool m_waitInterrupted{false};
    mutable QAbstractSocket::SocketError m_lastSocketError{QAbstractSocket::UnknownSocketError};
    mutable std::string m_lastErrorString;
};

}  // namespace media
