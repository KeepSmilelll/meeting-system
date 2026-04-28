import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Controls.Pane {
    id: root
    required property var controller
    readonly property bool hasController: root.controller !== null && root.controller !== undefined

    implicitWidth: 340
    implicitHeight: 360
    padding: 16

    background: Rectangle {
        radius: 20
        color: "#0b1220"
        border.color: "#1f2937"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Controls.Label {
                text: "Chat"
                color: "#f8fafc"
                font.pixelSize: 18
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            Controls.Label {
                text: root.hasController && root.controller.inMeeting ? "live" : "offline"
                color: root.hasController && root.controller.inMeeting ? "#22c55e" : "#64748b"
                font.pixelSize: 12
            }
        }

        ListView {
            id: messageList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 10
            model: root.hasController ? root.controller.chatMessageModel : null

            delegate: Item {
                width: ListView.view.width
                height: bubble.implicitHeight

                Rectangle {
                    id: bubble
                    height: messageColumn.implicitHeight + 24
                    width: Math.min(parent.width * 0.86, messageColumn.implicitWidth + 24)
                    x: local ? parent.width - width : 0
                    radius: 16
                    color: local ? "#0f766e" : "#111827"
                    border.color: local ? "#14b8a6" : "#1f2937"

                    ColumnLayout {
                        id: messageColumn
                        width: Math.min(messageText.implicitWidth, messageList.width * 0.74)
                        anchors.margins: 12
                        anchors.fill: parent
                        spacing: 4

                        Controls.Label {
                            Layout.fillWidth: true
                            text: senderName || senderId
                            color: local ? "#ccfbf1" : "#93c5fd"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }

                        Controls.Label {
                            id: messageText
                            Layout.fillWidth: true
                            text: content
                            color: "#f8fafc"
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            text: sentAt > 0 ? Qt.formatTime(new Date(sentAt), "HH:mm:ss") : ""
                            color: local ? "#99f6e4" : "#64748b"
                            font.pixelSize: 10
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }

            onCountChanged: Qt.callLater(function() {
                if (messageList.count > 0) {
                    messageList.positionViewAtEnd()
                }
            })

            Controls.Label {
                anchors.centerIn: parent
                visible: messageList.count === 0
                text: "No messages yet."
                color: "#64748b"
                font.pixelSize: 12
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Controls.TextField {
                id: messageInput
                Layout.fillWidth: true
                placeholderText: root.hasController && root.controller.inMeeting ? "Type a message" : "Join a meeting to chat"
                enabled: root.hasController && root.controller.inMeeting
                onAccepted: sendButton.clicked()
            }

            Controls.Button {
                id: sendButton
                text: "Send"
                enabled: root.hasController && root.controller.inMeeting && messageInput.text.trim().length > 0
                onClicked: {
                    if (!root.hasController) {
                        return
                    }
                    const text = messageInput.text.trim()
                    if (text.length === 0) {
                        return
                    }
                    root.controller.sendChat(text)
                    messageInput.clear()
                }
            }
        }
    }
}
