#include <cassert>

#include <QCoreApplication>

#include "../src/app/MediaSessionManager.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    MediaSessionManager manager;

    int endpointChanges = 0;
    int negotiationChanges = 0;
    QObject::connect(&manager, &MediaSessionManager::endpointChanged, &app, [&endpointChanges]() {
        ++endpointChanges;
    });
    QObject::connect(&manager, &MediaSessionManager::negotiationStateChanged, &app, [&negotiationChanges]() {
        ++negotiationChanges;
    });

    manager.setLocalUserId(QStringLiteral(" host-1 "));
    manager.setMeetingId(QStringLiteral(" meet-1 "));
    manager.setLocalHost(QStringLiteral("10.0.0.4"));
    manager.setLocalAudioPort(6200);
    manager.setAudioPayloadType(111);
    manager.setLocalVideoPort(7200);
    manager.setVideoPayloadType(97);
    manager.setLocalAudioSsrc(1111);
    manager.setLocalVideoSsrc(2222);
    manager.setAudioNegotiationEnabled(false);
    manager.setVideoNegotiationEnabled(false);

    assert(manager.localUserId() == QStringLiteral("host-1"));
    assert(manager.meetingId() == QStringLiteral("meet-1"));
    assert(manager.localHost() == QStringLiteral("10.0.0.4"));
    assert(manager.localAudioPort() == 6200);
    assert(manager.audioPayloadType() == 111);
    assert(manager.localVideoPort() == 7200);
    assert(manager.videoPayloadType() == 97);
    assert(manager.localAudioSsrc() == 1111);
    assert(manager.localVideoSsrc() == 2222);
    assert(!manager.audioNegotiationEnabled());
    assert(!manager.videoNegotiationEnabled());
    assert(endpointChanges >= 6);

    manager.reset();
    assert(manager.localAudioSsrc() == 0);
    assert(manager.localVideoSsrc() == 0);
    assert(manager.audioNegotiationEnabled());
    assert(manager.videoNegotiationEnabled());
    assert(negotiationChanges == 1);

    manager.setLocalHost(QString());
    manager.setLocalPort(0);
    manager.setPayloadType(120);
    assert(manager.localHost() == QStringLiteral("127.0.0.1"));
    assert(manager.localPort() == 5004);
    assert(manager.payloadType() == 120);

    return 0;
}
