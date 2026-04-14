#include <cassert>

#include <QCoreApplication>

#include "app/EventBus.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    EventBus bus;

    int wildcardCount = 0;
    int meetingCount = 0;
    int signalCount = 0;

    const int wildcardId = bus.subscribe(QString(), [&](const QString&, const QVariantMap&) {
        ++wildcardCount;
    });
    const int meetingId = bus.subscribe(QStringLiteral("meeting.updated"), [&](const QString& topic, const QVariantMap& payload) {
        assert(topic == QStringLiteral("meeting.updated"));
        assert(payload.value(QStringLiteral("meeting_id")).toString() == QStringLiteral("m-001"));
        ++meetingCount;
    });

    QObject::connect(&bus, &EventBus::eventPublished, [&](const QString&, const QVariantMap&) {
        ++signalCount;
    });

    QVariantMap payload;
    payload.insert(QStringLiteral("meeting_id"), QStringLiteral("m-001"));
    bus.publish(QStringLiteral("meeting.updated"), payload);

    assert(wildcardCount == 1);
    assert(meetingCount == 1);
    assert(signalCount == 1);

    assert(bus.unsubscribe(meetingId));
    bus.publish(QStringLiteral("meeting.updated"), payload);

    assert(wildcardCount == 2);
    assert(meetingCount == 1);
    assert(signalCount == 2);

    bus.clearSubscribers();
    bus.publish(QStringLiteral("meeting.updated"), payload);

    assert(wildcardCount == 2);
    assert(signalCount == 3);
    assert(wildcardId > 0);

    return 0;
}
