import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import "components"
import "dialogs"
import "pages"

Controls.ApplicationWindow {
    id: rootWindow
    required property var meetingController
    required property var appStateMachine
    visible: true
    width: 1280
    height: 820
    minimumWidth: 1024
    minimumHeight: 720
    title: "Meeting Client"
    color: "#08111f"

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#08111f" }
            GradientStop { position: 1.0; color: "#0f172a" }
        }
    }

    Rectangle {
        x: -140
        y: -100
        width: 360
        height: 360
        radius: 180
        color: "#0f766e"
        opacity: 0.18
    }

    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: -120
        anchors.bottomMargin: -100
        width: 420
        height: 420
        radius: 210
        color: "#f59e0b"
        opacity: 0.10
    }

    CreateMeetingDialog {
        id: createMeetingDialog
        controller: meetingController
    }

    JoinMeetingDialog {
        id: joinMeetingDialog
        controller: meetingController
    }

    Controls.Popup {
        id: settingsPopup
        modal: true
        focus: true
        width: Math.min(rootWindow.width - 64, 900)
        height: Math.min(rootWindow.height - 80, 620)
        anchors.centerIn: parent
        padding: 0
        closePolicy: Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 26
            color: "#020617"
            border.color: "#334155"
        }

        SettingsPage {
            anchors.fill: parent
            controller: meetingController
        }
    }

    Connections {
        target: meetingController

        function syncState() {
            appStateMachine.update(meetingController.connected, meetingController.loggedIn, meetingController.inMeeting)
            appStateMachine.reconnecting = meetingController.reconnecting
        }

        function onConnectedChanged() {
            syncState()
        }

        function onLoggedInChanged() {
            syncState()
        }

        function onInMeetingChanged() {
            syncState()
        }

        function onReconnectingChanged() {
            syncState()
        }

        Component.onCompleted: syncState()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TitleBar {
            window: rootWindow
            Layout.fillWidth: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: appStateMachine.reconnecting ? 34 : 0
            visible: appStateMachine.reconnecting
            color: "#7c2d12"
            border.color: "#f59e0b"

            Controls.Label {
                anchors.centerIn: parent
                text: "Connection lost, retrying..."
                color: "#fef3c7"
                font.pixelSize: 13
            }
        }

        StackLayout {
            id: contentStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: appStateMachine.inMeeting ? 2 : (appStateMachine.loggedIn ? 1 : 0)

            LoginDialog {
                controller: meetingController
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            HomePage {
                controller: meetingController
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            MeetingRoom {
                controller: meetingController
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: "#0f172a"
            border.color: "#1e293b"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                Controls.Label {
                    text: appStateMachine.stateName + " · " + meetingController.statusText
                    color: "#dbeafe"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Controls.Button {
                    visible: appStateMachine.loggedIn && !appStateMachine.inMeeting && !appStateMachine.reconnecting
                    text: "Create meeting"
                    onClicked: createMeetingDialog.open()
                }

                Controls.Button {
                    visible: appStateMachine.loggedIn && !appStateMachine.inMeeting && !appStateMachine.reconnecting
                    text: "Join meeting"
                    onClicked: joinMeetingDialog.open()
                }

                Controls.Button {
                    text: "Settings"
                    onClicked: settingsPopup.open()
                }

                Controls.Label {
                    text: appStateMachine.inMeeting ? "Meeting" : (appStateMachine.loggedIn ? "Home" : "Login")
                    color: "#94a3b8"
                    font.pixelSize: 12
                }
            }
        }
    }
}

