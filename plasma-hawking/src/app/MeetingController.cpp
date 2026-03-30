#include "MeetingController.h"

#include <QUuid>

MeetingController::MeetingController(QObject* parent)
    : QObject(parent) {}

bool MeetingController::inMeeting() const {
    return m_inMeeting;
}

bool MeetingController::audioMuted() const {
    return m_audioMuted;
}

bool MeetingController::videoMuted() const {
    return m_videoMuted;
}

QString MeetingController::meetingId() const {
    return m_meetingId;
}

void MeetingController::createMeeting(const QString& title, const QString& password) {
    Q_UNUSED(title)
    Q_UNUSED(password)

    m_meetingId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    m_inMeeting = true;
    m_audioMuted = false;
    m_videoMuted = false;

    emit meetingIdChanged();
    emit inMeetingChanged();
    emit audioMutedChanged();
    emit videoMutedChanged();
    emit infoMessage(QStringLiteral("Meeting created: %1").arg(m_meetingId));
}

void MeetingController::joinMeeting(const QString& meetingId, const QString& password) {
    Q_UNUSED(password)
    m_meetingId = meetingId;
    m_inMeeting = !m_meetingId.isEmpty();

    emit meetingIdChanged();
    emit inMeetingChanged();

    if (m_inMeeting) {
        emit infoMessage(QStringLiteral("Joined meeting: %1").arg(m_meetingId));
    }
}

void MeetingController::leaveMeeting() {
    if (!m_inMeeting) {
        return;
    }

    m_inMeeting = false;
    m_audioMuted = false;
    m_videoMuted = false;
    m_meetingId.clear();

    emit inMeetingChanged();
    emit audioMutedChanged();
    emit videoMutedChanged();
    emit meetingIdChanged();
    emit infoMessage(QStringLiteral("Left meeting"));
}

void MeetingController::toggleAudio() {
    if (!m_inMeeting) {
        return;
    }
    m_audioMuted = !m_audioMuted;
    emit audioMutedChanged();
}

void MeetingController::toggleVideo() {
    if (!m_inMeeting) {
        return;
    }
    m_videoMuted = !m_videoMuted;
    emit videoMutedChanged();
}
