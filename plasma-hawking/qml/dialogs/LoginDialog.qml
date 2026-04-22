import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Item {
    id: root
    required property var controller
    readonly property bool hasController: root.controller !== null && root.controller !== undefined

    implicitWidth: 520
    implicitHeight: 420

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#0f172a" }
            GradientStop { position: 1.0; color: "#111827" }
        }
    }

    Controls.Pane {
        width: Math.min(480, Math.max(360, root.width - 48))
        anchors.centerIn: parent
        padding: 28
        background: Rectangle {
            radius: 24
            color: "#0b1220"
            border.color: "#1f2937"
            border.width: 1
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 18

            ColumnLayout {
                spacing: 6
                Layout.fillWidth: true

                Controls.Label {
                    text: "Sign in"
                    color: "#f8fafc"
                    font.pixelSize: 28
                    font.bold: true
                }

                Controls.Label {
                    text: "Use your meeting account to continue."
                    color: "#94a3b8"
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                }
            }

            Controls.TextField {
                id: usernameField
                Layout.fillWidth: true
                placeholderText: "Username"
                text: root.hasController ? root.controller.username : ""
                enabled: root.hasController && !root.controller.reconnecting
                selectByMouse: true
                onAccepted: passwordField.forceActiveFocus()
            }

            Controls.TextField {
                id: passwordField
                Layout.fillWidth: true
                placeholderText: "Password"
                echoMode: TextInput.Password
                enabled: root.hasController && !root.controller.reconnecting
                selectByMouse: true
                onAccepted: {
                    if (root.hasController) {
                        root.controller.login(usernameField.text, passwordField.text)
                    }
                }
            }

            Controls.Button {
                id: signInButton
                Layout.fillWidth: true
                text: root.hasController && root.controller.reconnecting ? "Connecting..." : "Sign in"
                enabled: root.hasController && !root.controller.reconnecting
                onClicked: {
                    if (root.hasController) {
                        root.controller.login(usernameField.text, passwordField.text)
                    }
                }
            }

            Controls.Label {
                Layout.fillWidth: true
                text: root.hasController ? root.controller.statusText : "Shutting down"
                color: "#93c5fd"
                wrapMode: Text.WordWrap
            }
        }
    }
}

