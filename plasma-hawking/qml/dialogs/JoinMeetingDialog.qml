import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Controls.Dialog {
    id: root
    required property var controller
    readonly property bool hasController: root.controller !== null && root.controller !== undefined
    property bool joining: false
    readonly property bool validMeetingId: /^[0-9]{6,8}$/.test(meetingIdField.text.trim())

    modal: true
    dim: true
    width: 500
    title: "Join meeting"

    function submit() {
        if (!root.hasController || root.joining) {
            return
        }
        if (!root.validMeetingId) {
            errorLabel.text = "Meeting ID must be 6-8 digits."
            meetingIdField.forceActiveFocus()
            return
        }

        errorLabel.text = ""
        root.joining = true
        root.controller.joinMeeting(meetingIdField.text.trim(), passwordField.text)
    }

    onOpened: {
        root.joining = false
        errorLabel.text = ""
        meetingIdField.forceActiveFocus()
    }

    onClosed: {
        root.joining = false
    }

    background: Rectangle {
        radius: 24
        color: "#0b1220"
        border.color: "#1f2937"
        border.width: 1
    }

    Connections {
        target: root.hasController ? root.controller : null

        function onInMeetingChanged() {
            if (root.joining && root.controller.inMeeting) {
                root.joining = false
                root.close()
            }
        }

        function onStatusTextChanged() {
            if (!root.joining || root.controller.statusText === "Joining meeting...") {
                return
            }
            if (root.controller.inMeeting) {
                root.joining = false
                root.close()
                return
            }
            root.joining = false
            errorLabel.text = root.controller.statusText
        }
    }

    contentItem: Item {
        implicitWidth: 500
        implicitHeight: 340

        Controls.Pane {
            anchors.fill: parent
            padding: 24
            background: Rectangle {
                color: "transparent"
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 16

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Controls.Label {
                        text: "Join by meeting ID"
                        color: "#f8fafc"
                        font.pixelSize: 26
                        font.bold: true
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        text: "Enter the 6-8 digit meeting ID. Password is optional."
                        color: "#94a3b8"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                }

                Controls.TextField {
                    id: meetingIdField
                    Layout.fillWidth: true
                    placeholderText: "Meeting ID"
                    inputMethodHints: Qt.ImhDigitsOnly
                    maximumLength: 8
                    selectByMouse: true
                    enabled: root.hasController && !root.joining && !root.controller.reconnecting
                    onTextEdited: errorLabel.text = ""
                    onAccepted: passwordField.forceActiveFocus()
                }

                Controls.TextField {
                    id: passwordField
                    Layout.fillWidth: true
                    placeholderText: "Optional password"
                    echoMode: TextInput.Password
                    selectByMouse: true
                    enabled: root.hasController && !root.joining && !root.controller.reconnecting
                    onAccepted: root.submit()
                }

                Controls.Label {
                    id: errorLabel
                    Layout.fillWidth: true
                    color: text.length > 0 ? "#fca5a5" : "#93c5fd"
                    text: ""
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                }

                Item {
                    Layout.fillHeight: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Controls.Label {
                        Layout.fillWidth: true
                        text: root.hasController ? root.controller.statusText : "Shutting down"
                        color: "#93c5fd"
                        elide: Text.ElideRight
                    }

                    Controls.Button {
                        text: "Cancel"
                        enabled: !root.joining
                        onClicked: root.close()
                    }

                    Controls.Button {
                        text: root.joining ? "Joining..." : "Join"
                        enabled: root.hasController &&
                                 !root.joining &&
                                 !root.controller.reconnecting &&
                                 root.validMeetingId
                        onClicked: root.submit()
                    }
                }
            }
        }
    }
}
