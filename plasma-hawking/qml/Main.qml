import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"

ApplicationWindow {
    id: rootWindow
    visible: true
    width: 960
    height: 640
    title: "Meeting Client"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TitleBar {
            window: rootWindow
            Layout.fillWidth: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            anchors.margins: 24
            spacing: 12

            Rectangle {
                Layout.fillWidth: true
                height: 40
                radius: 6
                visible: meetingController.reconnecting
                color: "#fef3c7"
                border.color: "#f59e0b"

                Text {
                    anchors.centerIn: parent
                    text: "Reconnecting..."
                    color: "#92400e"
                    font.pixelSize: 14
                }
            }

            GroupBox {
                Layout.fillWidth: true
                title: "Login"
                visible: !meetingController.loggedIn

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    TextField {
                        id: usernameInput
                        placeholderText: "Username"
                        text: "demo"
                        Layout.fillWidth: true
                    }

                    TextField {
                        id: passwordInput
                        placeholderText: "Password"
                        echoMode: TextInput.Password
                        text: "demo"
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        Button {
                            text: "Login"
                            onClicked: meetingController.login(usernameInput.text, passwordInput.text)
                        }
                    }
                }
            }

            GroupBox {
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: "Meeting"
                visible: meetingController.loggedIn

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    Text {
                        text: meetingController.inMeeting ? "In Meeting: " + meetingController.meetingId : "Not in meeting"
                        font.pixelSize: 20
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

                        Item {
                            Layout.fillWidth: true
                        }

                        Button {
                            text: "Logout"
                            onClicked: meetingController.logout()
                        }
                    }

                    GroupBox {
                        title: "Participants"
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ListView {
                            anchors.fill: parent
                            model: meetingController.participants
                            clip: true

                            delegate: Rectangle {
                                width: ListView.view.width
                                height: 36
                                color: index % 2 === 0 ? "#f5f7fa" : "#ffffff"

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 10
                                    text: modelData
                                    color: "#1f2937"
                                }
                            }
                        }
                    }
                }
            }

            Text {
                id: statusText
                text: meetingController.statusText
                color: "#5f6b76"
                font.pixelSize: 14
            }
        }
    }
}
