import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    visible: true
    width: 960
    height: 640
    title: "Meeting Client"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        Text {
            text: meetingController.inMeeting ? "In Meeting: " + meetingController.meetingId : "Not in meeting"
            font.pixelSize: 24
        }

        RowLayout {
            spacing: 8

            TextField {
                id: meetingInput
                placeholderText: "Meeting ID"
                Layout.preferredWidth: 220
            }

            Button {
                text: "Create"
                onClicked: meetingController.createMeeting("Quick Meeting", "")
            }

            Button {
                text: "Join"
                onClicked: meetingController.joinMeeting(meetingInput.text, "")
            }

            Button {
                text: "Leave"
                enabled: meetingController.inMeeting
                onClicked: meetingController.leaveMeeting()
            }
        }

        RowLayout {
            spacing: 8

            Button {
                text: meetingController.audioMuted ? "Unmute Audio" : "Mute Audio"
                enabled: meetingController.inMeeting
                onClicked: meetingController.toggleAudio()
            }

            Button {
                text: meetingController.videoMuted ? "Unmute Video" : "Mute Video"
                enabled: meetingController.inMeeting
                onClicked: meetingController.toggleVideo()
            }
        }

        Text {
            id: statusText
            text: "Ready"
            color: "#5f6b76"
            font.pixelSize: 14
        }

        Connections {
            target: meetingController
            function onInfoMessage(message) {
                statusText.text = message
            }
        }
    }
}
