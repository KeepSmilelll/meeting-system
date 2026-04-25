#include <QCoreApplication>

#include <cassert>
#include <iostream>

#include "net/media/DtlsTransportClient.h"
#include "server/DtlsContext.h"
#include "server/DtlsTransport.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    sfu::DtlsContext serverContext;
    const bool serverInitialized = serverContext.Initialize();
    if (!serverInitialized) {
        std::cerr << "serverContext.Initialize failed: " << serverContext.LastError() << "\n";
    }
    assert(serverInitialized);

    media::DtlsTransportClient client;
    QList<QByteArray> clientOutgoing;
    assert(client.start(QString::fromStdString(serverContext.FingerprintSha256()), &clientOutgoing));

    sfu::DtlsTransport server(serverContext, sfu::DtlsTransport::Role::Server);
    std::vector<std::vector<uint8_t>> serverOutgoing;
    assert(server.Start(client.localFingerprintSha256().toStdString(), &serverOutgoing));

    for (int iteration = 0; iteration < 64; ++iteration) {
        QList<QByteArray> nextClientOutgoing;
        std::vector<std::vector<uint8_t>> nextServerOutgoing;
        bool progressed = false;

        for (const QByteArray& packet : clientOutgoing) {
            assert(server.HandleIncomingDatagram(
                reinterpret_cast<const uint8_t*>(packet.constData()),
                static_cast<std::size_t>(packet.size()),
                &nextServerOutgoing));
            progressed = true;
        }
        for (const auto& packet : serverOutgoing) {
            assert(client.handleIncomingDatagram(
                QByteArray(reinterpret_cast<const char*>(packet.data()),
                           static_cast<int>(packet.size())),
                &nextClientOutgoing));
            progressed = true;
        }

        clientOutgoing = std::move(nextClientOutgoing);
        serverOutgoing = std::move(nextServerOutgoing);
        if (client.isConnected() && server.IsConnected()) {
            break;
        }
        assert(progressed);
    }

    assert(client.isConnected());
    assert(server.IsConnected());
    assert(client.selectedSrtpProfile() == QStringLiteral("SRTP_AES128_CM_SHA1_80"));
    assert(QString::fromStdString(server.SelectedSrtpProfile()) == QStringLiteral("SRTP_AES128_CM_SHA1_80"));
    assert(client.peerFingerprintSha256() == QString::fromStdString(serverContext.FingerprintSha256()));
    assert(QString::fromStdString(server.PeerFingerprintSha256()) == client.localFingerprintSha256());

    media::DtlsTransportClient::SrtpKeyMaterial clientKeying;
    sfu::DtlsTransport::SrtpKeyMaterial serverKeying;
    assert(client.exportSrtpKeyMaterial(16, 14, &clientKeying));
    assert(server.ExportSrtpKeyMaterial(16U, 14U, &serverKeying));
    assert(clientKeying.localKey ==
           QByteArray(reinterpret_cast<const char*>(serverKeying.remoteKey.data()),
                      static_cast<int>(serverKeying.remoteKey.size())));
    assert(clientKeying.remoteKey ==
           QByteArray(reinterpret_cast<const char*>(serverKeying.localKey.data()),
                      static_cast<int>(serverKeying.localKey.size())));
    assert(clientKeying.localSalt ==
           QByteArray(reinterpret_cast<const char*>(serverKeying.remoteSalt.data()),
                      static_cast<int>(serverKeying.remoteSalt.size())));
    assert(clientKeying.remoteSalt ==
           QByteArray(reinterpret_cast<const char*>(serverKeying.localSalt.data()),
                      static_cast<int>(serverKeying.localSalt.size())));

    return 0;
}
