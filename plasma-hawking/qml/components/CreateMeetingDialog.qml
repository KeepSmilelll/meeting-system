import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Controls.Dialog {
    id: root
    required property var controller

    modal: true
    dim: true
    width: 520
    title: "Create meeting"

    background: Rectangle {
        radius: 24
        color: "#0b1220"
        border.color: "#1f2937"
        border.width: 1
    }

    contentItem: Item {
        implicitWidth: 520
        implicitHeight: 420

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
                        text: "Create a meeting"
                        color: "#f8fafc"
                        font.pixelSize: 26
                        font.bold: true
                    }

                    Controls.Label {
                        text: "Enter a title, optional password, and the maximum participant count."
                        color: "#94a3b8"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                }

                Controls.TextField {
                    id: titleField
                    Layout.fillWidth: true
                    placeholderText: "Meeting title"
                    text: "Quick Meeting"
                    selectByMouse: true
                    enabled: !root.controller.reconnecting
                    onAccepted: passwordField.forceActiveFocus()
                }

                Controls.TextField {
                    id: passwordField
                    Layout.fillWidth: true
                    placeholderText: "Optional password"
                    echoMode: TextInput.Password
                    selectByMouse: true
                    enabled: !root.controller.reconnecting
                    onAccepted: maxParticipantsField.forceActiveFocus()
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Controls.Label {
                        text: "Maximum participants"
                        color: "#cbd5e1"
                        font.pixelSize: 12
                    }

                    Controls.SpinBox {
                        id: maxParticipantsField
                        Layout.fillWidth: true
                        from: 2
                        to: 256
                        value: 16
                        editable: true
                        enabled: !root.controller.reconnecting
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Item { Layout.fillWidth: true }

                    Controls.Button {
                        text: "Cancel"
                        enabled: !root.controller.reconnecting
                        onClicked: root.close()
                    }

                    Controls.Button {
                        id: confirmButton
                        text: root.controller.reconnecting ? "Creating..." : "Create"
                        enabled: !root.controller.reconnecting && titleField.text.trim().length > 0
                        onClicked: {
                            root.controller.createMeeting(titleField.text, passwordField.text, maxParticipantsField.value)
                            root.close()
                        }
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    text: root.controller.statusText
                    color: "#93c5fd"
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
