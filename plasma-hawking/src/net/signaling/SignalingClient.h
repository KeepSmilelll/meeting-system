#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <string>

class QTcpSocket;

namespace signaling {

class SignalingClient : public QObject {
    Q_OBJECT

public:
    explicit SignalingClient(QObject* parent = nullptr);
    ~SignalingClient() override;

    void connectToServer(const QString& host, quint16 port);
    void reconnect();
    void disconnectFromServer();

    bool isConnected() const;

    void login(const QString& username,
               const QString& passwordHash,
               const QString& deviceId,
               const QString& platform);
    void sendHeartbeat(qint64 clientTimestampMs = 0);
    void createMeeting(const QString& title, const QString& password, int maxParticipants);
    void joinMeeting(const QString& meetingId, const QString& password);
    void sendChat(int type, const QString& content, const QString& replyToId = {});

signals:
    void connectedChanged(bool connected);
    void protocolError(const QString& message);

    void loginFinished(bool success, const QString& userId, const QString& token, const QString& error);
    void heartbeatReceived(qint64 serverTimestampMs);
    void createMeetingFinished(bool success, const QString& meetingId, const QString& error);
    void joinMeetingFinished(bool success,
                             const QString& meetingId,
                             const QString& title,
                             const QStringList& participants,
                             const QString& error);
    void chatSendFinished(bool success, const QString& messageId, const QString& error);
    void chatReceived(const QString& senderId,
                      const QString& senderName,
                      int type,
                      const QString& content,
                      const QString& replyToId,
                      qint64 timestamp);

    // Forward-compatible escape hatch for unhandled protobuf messages.
    void protobufMessageReceived(quint16 signalType, const QByteArray& payload);

private:
    void processIncomingFrames();
    void handlePayload(quint16 signalType, const QByteArray& payload);
    void sendRawFrame(quint16 signalType, const std::string& payload);

    QTcpSocket* m_socket{nullptr};
    QByteArray m_readBuffer;
    quint32 m_maxPayloadBytes{4 * 1024 * 1024};

    QString m_host;
    quint16 m_port{0};
};

}  // namespace signaling
