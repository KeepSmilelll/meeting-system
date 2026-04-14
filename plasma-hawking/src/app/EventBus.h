#pragma once

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QVariantMap>

#include <functional>

class EventBus : public QObject {
    Q_OBJECT

public:
    using Subscriber = std::function<void(const QString& topic, const QVariantMap& payload)>;

    explicit EventBus(QObject* parent = nullptr);

    int subscribe(const QString& topic, Subscriber subscriber);
    bool unsubscribe(int subscriptionId);
    void clearSubscribers();

    Q_INVOKABLE void publish(const QString& topic, const QVariantMap& payload = QVariantMap{});

signals:
    void eventPublished(const QString& topic, const QVariantMap& payload);

private:
    struct Subscription {
        QString topic;
        Subscriber callback;
    };

    mutable QMutex m_mutex;
    QHash<int, Subscription> m_subscriptions;
    int m_nextSubscriptionId{1};
};
