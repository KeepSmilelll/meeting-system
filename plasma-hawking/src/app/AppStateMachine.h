#pragma once

#include <QObject>
#include <QString>

class AppStateMachine : public QObject {
    Q_OBJECT

public:
    enum class State {
        Disconnected = 0,
        Connected,
        LoggedIn,
        InMeeting,
    };
    Q_ENUM(State)

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString stateName READ stateName NOTIFY stateChanged)
    Q_PROPERTY(bool disconnected READ disconnected NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY stateChanged)
    Q_PROPERTY(bool inMeeting READ inMeeting NOTIFY stateChanged)
    Q_PROPERTY(bool reconnecting READ reconnecting WRITE setReconnecting NOTIFY reconnectingChanged)

    explicit AppStateMachine(QObject* parent = nullptr);

    State state() const;
    QString stateName() const;
    bool disconnected() const;
    bool connected() const;
    bool loggedIn() const;
    bool inMeeting() const;
    bool reconnecting() const;

public slots:
    void update(bool connected, bool loggedIn, bool inMeeting);
    void reset();
    void setReconnecting(bool reconnecting);

signals:
    void stateChanged();
    void reconnectingChanged();

private:
    void setState(State state);

    State m_state{State::Disconnected};
    bool m_connected{false};
    bool m_loggedIn{false};
    bool m_inMeeting{false};
    bool m_reconnecting{false};
};
