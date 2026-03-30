#include "SignalingClient.h"

#include "SignalProtocol.h"

#include <QDateTime>
#include <QTcpSocket>

#include <string>
#include <vector>

#include "signaling.pb.h"

namespace signaling {
namespace {

constexpr quint16 kAuthLoginReq = 0x0101;
constexpr quint16 kAuthLoginRsp = 0x0102;
constexpr quint16 kAuthHeartbeatReq = 0x0105;
constexpr quint16 kAuthHeartbeatRsp = 0x0106;
constexpr quint16 kMeetCreateReq = 0x0201;
constexpr quint16 kMeetCreateRsp = 0x0202;
constexpr quint16 kMeetJoinReq = 0x0203;
constexpr quint16 kMeetJoinRsp = 0x0204;
constexpr quint16 kChatSendReq = 0x0401;
constexpr quint16 kChatSendRsp = 0x0402;
constexpr quint16 kChatRecvNotify = 0x0403;

QString toQtString(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

QString protobufError(const meeting::ErrorInfo& err) {
    if (err.message().empty()) {
        return QStringLiteral("code=%1").arg(err.code());
    }
    return QStringLiteral("%1 (code=%2)").arg(toQtString(err.message())).arg(err.code());
}

}  // namespace

SignalingClient::SignalingClient(QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this)) {
    connect(m_socket, &QTcpSocket::connected, this, [this]() {
        emit connectedChanged(true);
    });

    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        emit connectedChanged(false);
    });

    connect(m_socket, &QTcpSocket::readyRead, this, [this]() {
        m_readBuffer.append(m_socket->readAll());
        processIncomingFrames();
    });

    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit protocolError(m_socket->errorString());
        if (m_socket->state() == QAbstractSocket::UnconnectedState) {
            emit connectedChanged(false);
        }
    });
}

SignalingClient::~SignalingClient() = default;

void SignalingClient::connectToServer(const QString& host, quint16 port) {
    m_host = host;
    m_port = port;

    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    m_socket->connectToHost(host, port);
}

void SignalingClient::reconnect() {
    if (m_host.isEmpty() || m_port == 0) {
        emit protocolError(QStringLiteral("No known server endpoint for reconnect"));
        return;
    }
    connectToServer(m_host, m_port);
}

void SignalingClient::disconnectFromServer() {
    m_socket->disconnectFromHost();
}

bool SignalingClient::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void SignalingClient::login(const QString& username,
                            const QString& passwordHash,
                            const QString& deviceId,
                            const QString& platform) {
    meeting::AuthLoginReq req;
    req.set_username(username.toStdString());
    req.set_password_hash(passwordHash.toStdString());
    req.set_device_id(deviceId.toStdString());
    req.set_platform(platform.toStdString());

    std::string payload;
    if (!req.SerializeToString(&payload)) {
        emit protocolError(QStringLiteral("Failed to serialize AuthLoginReq"));
        return;
    }

    sendRawFrame(kAuthLoginReq, payload);
}

void SignalingClient::sendHeartbeat(qint64 clientTimestampMs) {
    if (!isConnected()) {
        return;
    }

    if (clientTimestampMs <= 0) {
        clientTimestampMs = QDateTime::currentMSecsSinceEpoch();
    }

    meeting::AuthHeartbeatReq req;
    req.set_client_timestamp(clientTimestampMs);

    std::string payload;
    if (!req.SerializeToString(&payload)) {
        emit protocolError(QStringLiteral("Failed to serialize AuthHeartbeatReq"));
        return;
    }

    sendRawFrame(kAuthHeartbeatReq, payload);
}

void SignalingClient::createMeeting(const QString& title, const QString& password, int maxParticipants) {
    meeting::MeetCreateReq req;
    req.set_title(title.toStdString());
    req.set_password(password.toStdString());
    req.set_max_participants(maxParticipants);

    std::string payload;
    if (!req.SerializeToString(&payload)) {
        emit protocolError(QStringLiteral("Failed to serialize MeetCreateReq"));
        return;
    }

    sendRawFrame(kMeetCreateReq, payload);
}

void SignalingClient::joinMeeting(const QString& meetingId, const QString& password) {
    meeting::MeetJoinReq req;
    req.set_meeting_id(meetingId.toStdString());
    req.set_password(password.toStdString());

    std::string payload;
    if (!req.SerializeToString(&payload)) {
        emit protocolError(QStringLiteral("Failed to serialize MeetJoinReq"));
        return;
    }

    sendRawFrame(kMeetJoinReq, payload);
}

void SignalingClient::sendChat(int type, const QString& content, const QString& replyToId) {
    meeting::ChatSendReq req;
    req.set_type(type);
    req.set_content(content.toStdString());
    if (!replyToId.isEmpty()) {
        req.set_reply_to_id(replyToId.toStdString());
    }

    std::string payload;
    if (!req.SerializeToString(&payload)) {
        emit protocolError(QStringLiteral("Failed to serialize ChatSendReq"));
        return;
    }

    sendRawFrame(kChatSendReq, payload);
}

void SignalingClient::processIncomingFrames() {
    while (m_readBuffer.size() >= static_cast<int>(kHeaderSize)) {
        const QByteArray headerBytes = m_readBuffer.left(static_cast<int>(kHeaderSize));
        std::vector<uint8_t> header;
        header.reserve(kHeaderSize);
        for (const char b : headerBytes) {
            header.push_back(static_cast<uint8_t>(b));
        }

        const auto decoded = decodeHeader(header, m_maxPayloadBytes);
        if (!decoded.has_value()) {
            emit protocolError(QStringLiteral("Invalid signaling frame header"));
            m_socket->disconnectFromHost();
            m_readBuffer.clear();
            return;
        }

        const auto frameSize = static_cast<int>(kHeaderSize + decoded->length);
        if (m_readBuffer.size() < frameSize) {
            return;
        }

        const QByteArray payload = m_readBuffer.mid(static_cast<int>(kHeaderSize), static_cast<int>(decoded->length));
        m_readBuffer.remove(0, frameSize);

        handlePayload(decoded->type, payload);
    }
}

void SignalingClient::handlePayload(quint16 signalType, const QByteArray& payload) {
    switch (signalType) {
    case kAuthLoginRsp: {
        meeting::AuthLoginRsp rsp;
        if (!rsp.ParseFromArray(payload.constData(), payload.size())) {
            emit protocolError(QStringLiteral("Failed to parse AuthLoginRsp"));
            return;
        }

        const QString err = rsp.has_error() ? protobufError(rsp.error()) : QString();
        emit loginFinished(rsp.success(), toQtString(rsp.user_id()), toQtString(rsp.token()), err);
        return;
    }
    case kAuthHeartbeatRsp: {
        meeting::AuthHeartbeatRsp rsp;
        if (!rsp.ParseFromArray(payload.constData(), payload.size())) {
            emit protocolError(QStringLiteral("Failed to parse AuthHeartbeatRsp"));
            return;
        }

        emit heartbeatReceived(static_cast<qint64>(rsp.server_timestamp()));
        return;
    }
    case kMeetCreateRsp: {
        meeting::MeetCreateRsp rsp;
        if (!rsp.ParseFromArray(payload.constData(), payload.size())) {
            emit protocolError(QStringLiteral("Failed to parse MeetCreateRsp"));
            return;
        }

        const QString err = rsp.has_error() ? protobufError(rsp.error()) : QString();
        emit createMeetingFinished(rsp.success(), toQtString(rsp.meeting_id()), err);
        return;
    }
    case kMeetJoinRsp: {
        meeting::MeetJoinRsp rsp;
        if (!rsp.ParseFromArray(payload.constData(), payload.size())) {
            emit protocolError(QStringLiteral("Failed to parse MeetJoinRsp"));
            return;
        }

        QStringList participants;
        participants.reserve(rsp.participants_size());
        for (const auto& p : rsp.participants()) {
            participants.push_back(QStringLiteral("%1 (%2)").arg(toQtString(p.display_name()), toQtString(p.user_id())));
        }

        const QString err = rsp.has_error() ? protobufError(rsp.error()) : QString();
        emit joinMeetingFinished(rsp.success(), toQtString(rsp.meeting_id()), toQtString(rsp.title()), participants, err);
        return;
    }
    case kChatSendRsp: {
        meeting::ChatSendRsp rsp;
        if (!rsp.ParseFromArray(payload.constData(), payload.size())) {
            emit protocolError(QStringLiteral("Failed to parse ChatSendRsp"));
            return;
        }

        const QString err = rsp.has_error() ? protobufError(rsp.error()) : QString();
        emit chatSendFinished(rsp.success(), toQtString(rsp.message_id()), err);
        return;
    }
    case kChatRecvNotify: {
        meeting::ChatRecvNotify notify;
        if (!notify.ParseFromArray(payload.constData(), payload.size())) {
            emit protocolError(QStringLiteral("Failed to parse ChatRecvNotify"));
            return;
        }

        emit chatReceived(toQtString(notify.sender_id()),
                          toQtString(notify.sender_name()),
                          notify.type(),
                          toQtString(notify.content()),
                          toQtString(notify.reply_to_id()),
                          static_cast<qint64>(notify.timestamp()));
        return;
    }
    default:
        emit protobufMessageReceived(signalType, payload);
        return;
    }
}

void SignalingClient::sendRawFrame(quint16 signalType, const std::string& payload) {
    if (!isConnected()) {
        emit protocolError(QStringLiteral("Socket is not connected"));
        return;
    }

    const std::vector<uint8_t> payloadBytes(payload.begin(), payload.end());
    const auto frame = encodeFrame(static_cast<uint16_t>(signalType), payloadBytes);

    if (m_socket->write(reinterpret_cast<const char*>(frame.data()), static_cast<qint64>(frame.size())) < 0) {
        emit protocolError(m_socket->errorString());
    }
}

}  // namespace signaling
