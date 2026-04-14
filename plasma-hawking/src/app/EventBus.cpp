#include "EventBus.h"

#include <QMutexLocker>
#include <QVector>

EventBus::EventBus(QObject* parent)
    : QObject(parent) {}

int EventBus::subscribe(const QString& topic, Subscriber subscriber) {
    if (!subscriber) {
        return 0;
    }

    QMutexLocker lock(&m_mutex);
    const int subscriptionId = m_nextSubscriptionId++;
    m_subscriptions.insert(subscriptionId, Subscription{topic.trimmed(), std::move(subscriber)});
    return subscriptionId;
}

bool EventBus::unsubscribe(int subscriptionId) {
    if (subscriptionId <= 0) {
        return false;
    }

    QMutexLocker lock(&m_mutex);
    if (!m_subscriptions.contains(subscriptionId)) {
        return false;
    }

    m_subscriptions.remove(subscriptionId);
    return true;
}

void EventBus::clearSubscribers() {
    QMutexLocker lock(&m_mutex);
    m_subscriptions.clear();
}

void EventBus::publish(const QString& topic, const QVariantMap& payload) {
    const QString normalizedTopic = topic.trimmed();
    QVector<Subscriber> callbacks;

    {
        QMutexLocker lock(&m_mutex);
        callbacks.reserve(m_subscriptions.size());
        for (auto it = m_subscriptions.cbegin(); it != m_subscriptions.cend(); ++it) {
            const Subscription& subscription = it.value();
            if (!subscription.callback) {
                continue;
            }
            if (!subscription.topic.isEmpty() && subscription.topic != normalizedTopic) {
                continue;
            }
            callbacks.push_back(subscription.callback);
        }
    }

    for (const auto& callback : callbacks) {
        callback(normalizedTopic, payload);
    }

    emit eventPublished(normalizedTopic, payload);
}
