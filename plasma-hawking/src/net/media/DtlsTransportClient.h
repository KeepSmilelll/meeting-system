#pragma once

#include <QByteArray>
#include <QString>

#include <memory>

namespace media {

class DtlsTransportClient final {
public:
    struct SrtpKeyMaterial {
        QByteArray localKey;
        QByteArray remoteKey;
        QByteArray localSalt;
        QByteArray remoteSalt;
    };

    DtlsTransportClient();
    ~DtlsTransportClient();

    DtlsTransportClient(const DtlsTransportClient&) = delete;
    DtlsTransportClient& operator=(const DtlsTransportClient&) = delete;
    DtlsTransportClient(DtlsTransportClient&&) noexcept;
    DtlsTransportClient& operator=(DtlsTransportClient&&) noexcept;

    bool prepareLocalFingerprint();
    bool start(const QString& expectedServerFingerprint,
               QList<QByteArray>* outgoingPackets = nullptr);
    bool handleIncomingDatagram(const QByteArray& datagram,
                                QList<QByteArray>* outgoingPackets = nullptr);

    bool isStarted() const;
    bool isConnected() const;
    QString localFingerprintSha256() const;
    QString peerFingerprintSha256() const;
    QString selectedSrtpProfile() const;
    QString lastError() const;

    bool exportSrtpKeyMaterial(int keyLen, int saltLen, SrtpKeyMaterial* out) const;

    static bool looksLikeDtlsRecord(const QByteArray& datagram);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace media
