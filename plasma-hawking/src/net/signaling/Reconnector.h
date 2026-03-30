#pragma once

#include <QObject>
#include <QTimer>

namespace signaling {

class Reconnector : public QObject {
    Q_OBJECT

public:
    explicit Reconnector(QObject* parent = nullptr);

    void configure(int initialDelayMs, int maxDelayMs);
    void schedule();
    void reset();
    void stop();

    bool active() const;

signals:
    void reconnectRequested();
    void activeChanged(bool active);

private:
    QTimer m_timer;
    int m_initialDelayMs{1000};
    int m_maxDelayMs{30000};
    int m_nextDelayMs{1000};
};

}  // namespace signaling
