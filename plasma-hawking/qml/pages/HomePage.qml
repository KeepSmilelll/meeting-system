import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Item {
    id: root
    required property var controller
    readonly property bool hasController: root.controller !== null && root.controller !== undefined
    readonly property int participantCount: root.hasController ? root.controller.participants.length : 0

    implicitWidth: 1100
    implicitHeight: 760

    Rectangle {
        anchors.fill: parent
        color: "transparent"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18

        Controls.Pane {
            Layout.fillWidth: true
            padding: 20
            background: Rectangle {
                radius: 22
                color: "#0b1220"
                border.color: "#1f2937"
            }

            RowLayout {
                anchors.fill: parent
                spacing: 18

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Controls.Label {
                        text: "Welcome, %1".arg(root.hasController ? (root.controller.username || "guest") : "guest")
                        color: "#f8fafc"
                        font.pixelSize: 28
                        font.bold: true
                    }

                    Controls.Label {
                        text: !root.hasController
                              ? "Session view is closing."
                              : ((root.controller.userId === "") ? "Ready to create or join a meeting." : "User ID: " + root.controller.userId)
                        color: "#94a3b8"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    spacing: 6

                    Controls.Label {
                        text: root.hasController && root.controller.loggedIn ? "Connected" : "Offline"
                        color: root.hasController && root.controller.loggedIn ? "#22c55e" : "#f87171"
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Controls.Label {
                        text: root.hasController ? root.controller.statusText : "Shutting down"
                        color: "#cbd5e1"
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: root.width >= 980 ? 2 : 1
            columnSpacing: 18
            rowSpacing: 18

            Controls.Pane {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 20
                background: Rectangle {
                    radius: 20
                    color: "#0f172a"
                    border.color: "#1e293b"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 14

                    Controls.Label {
                        text: "Create a meeting"
                        color: "#e2e8f0"
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Controls.Label {
                        text: "Start a new room and become the host."
                        color: "#94a3b8"
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }

                    Controls.TextField {
                        id: createTitleField
                        Layout.fillWidth: true
                        placeholderText: "Meeting title"
                        text: "Quick Meeting"
                    }

                    Controls.TextField {
                        id: createPasswordField
                        Layout.fillWidth: true
                        placeholderText: "Optional password"
                        echoMode: TextInput.Password
                    }

                    Item {
                        Layout.fillHeight: true
                    }

                    Controls.Button {
                        Layout.fillWidth: true
                        text: "Create meeting"
                        enabled: root.hasController && root.controller.loggedIn && !root.controller.reconnecting
                        onClicked: {
                            if (root.hasController) {
                                root.controller.createMeeting(createTitleField.text, createPasswordField.text)
                            }
                        }
                    }
                }
            }

            Controls.Pane {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 20
                background: Rectangle {
                    radius: 20
                    color: "#0f172a"
                    border.color: "#1e293b"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 14

                    Controls.Label {
                        text: "Join a meeting"
                        color: "#e2e8f0"
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Controls.Label {
                        text: "Enter a meeting ID and optional password."
                        color: "#94a3b8"
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }

                    Controls.TextField {
                        id: meetingIdField
                        Layout.fillWidth: true
                        placeholderText: "Meeting ID"
                    }

                    Controls.TextField {
                        id: joinPasswordField
                        Layout.fillWidth: true
                        placeholderText: "Optional password"
                        echoMode: TextInput.Password
                    }

                    Item {
                        Layout.fillHeight: true
                    }

                    Controls.Button {
                        Layout.fillWidth: true
                        text: "Join meeting"
                        enabled: root.hasController && root.controller.loggedIn && !root.controller.reconnecting
                        onClicked: {
                            if (root.hasController) {
                                root.controller.joinMeeting(meetingIdField.text, joinPasswordField.text)
                            }
                        }
                    }
                }
            }
        }

        Controls.Pane {
            Layout.fillWidth: true
            padding: 18
            background: Rectangle {
                radius: 20
                color: "#0b1220"
                border.color: "#1e293b"
            }

            RowLayout {
                anchors.fill: parent
                spacing: 16

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Controls.Label {
                        text: "Session"
                        color: "#e2e8f0"
                        font.pixelSize: 16
                        font.bold: true
                    }

                    Controls.Label {
                        text: root.participantCount > 0 ? "Participants: " + root.participantCount : "No active meeting members yet."
                        color: "#94a3b8"
                        font.pixelSize: 12
                    }
                }

                Controls.Button {
                    text: "Logout"
                    enabled: root.hasController && root.controller.loggedIn
                    onClicked: {
                        if (root.hasController) {
                            root.controller.logout()
                        }
                    }
                }
            }
        }
    }
}


