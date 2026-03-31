#include "AppStateMachine.h"

AppStateMachine::AppStateMachine(QObject* parent)
    : QObject(parent) {}

AppStateMachine::State AppStateMachine::state() const {
    return m_state;
}

QString AppStateMachine::stateName() const {
    switch (m_state) {
    case State::Disconnected:
        return QStringLiteral("Disconnected");
    case State::Connected:
        return QStringLiteral("Connected");
    case State::LoggedIn:
        return QStringLiteral("LoggedIn");
    case State::InMeeting:
        return QStringLiteral("InMeeting");
    }
    return QStringLiteral("Disconnected");
}

bool AppStateMachine::disconnected() const {
    return m_state == State::Disconnected;
}

bool AppStateMachine::connected() const {
    return m_connected;
}

bool AppStateMachine::loggedIn() const {
    return m_loggedIn;
}

bool AppStateMachine::inMeeting() const {
    return m_inMeeting;
}

bool AppStateMachine::reconnecting() const {
    return m_reconnecting;
}

void AppStateMachine::update(bool connected, bool loggedIn, bool inMeeting) {
    m_connected = connected;
    m_loggedIn = loggedIn;
    m_inMeeting = inMeeting;

    State nextState = State::Disconnected;
    if (m_inMeeting) {
        nextState = State::InMeeting;
    } else if (m_loggedIn) {
        nextState = State::LoggedIn;
    } else if (m_connected) {
        nextState = State::Connected;
    }

    setState(nextState);
}

void AppStateMachine::reset() {
    m_connected = false;
    m_loggedIn = false;
    m_inMeeting = false;
    setState(State::Disconnected);
    setReconnecting(false);
}

void AppStateMachine::setReconnecting(bool reconnecting) {
    if (m_reconnecting == reconnecting) {
        return;
    }

    m_reconnecting = reconnecting;
    emit reconnectingChanged();
}

void AppStateMachine::setState(State state) {
    if (m_state == state) {
        return;
    }

    m_state = state;
    emit stateChanged();
}
