import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Controls.Pane {
    id: root
    required property var controller

    implicitHeight: 82
    padding: 16

    background: Rectangle {
        radius: 20
        color: "#0b1220"
        border.color: "#1f2937"
    }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        Controls.Button {
            text: root.controller.audioMuted ? "Unmute mic" : "Mute mic"
            enabled: root.controller.inMeeting
            onClicked: root.controller.toggleAudio()
        }

        Controls.Button {
            text: root.controller.videoMuted ? "Unmute camera" : "Mute camera"
            enabled: root.controller.inMeeting
            onClicked: root.controller.toggleVideo()
        }

        Item {
            Layout.fillWidth: true
        }

        ColumnLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: 2

            Controls.Label {
                text: root.controller.meetingTitle !== ""
                          ? root.controller.meetingTitle + (root.controller.meetingId !== "" ? " (" + root.controller.meetingId + ")" : "")
                          : ((root.controller.meetingId === "") ? "No meeting selected" : "Meeting ID: " + root.controller.meetingId)
                color: "#e2e8f0"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignRight
            }

            Controls.Label {
                text: root.controller.statusText
                color: "#94a3b8"
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
            }
        }

        Controls.Button {
            text: "Leave"
            enabled: root.controller.inMeeting
            onClicked: root.controller.leaveMeeting()
        }
    }
}
