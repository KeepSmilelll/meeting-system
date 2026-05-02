import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Controls.Pane {
    id: root
    required property var controller
    readonly property bool hasController: root.controller !== null && root.controller !== undefined
    property var searchResults: []

    implicitWidth: 340
    implicitHeight: 360
    padding: 16

    function scheduleBubbleLayoutSmokeCheck() {
        if (!root.hasController
                || !root.controller.runtimeSmokeRequiresChatBubbleLayoutEvidence()
                || messageList.smokeBubbleLayoutReported) {
            return
        }
        bubbleLayoutSmokeTimer.restart()
    }

    function reportBubbleLayoutSmokeEvidence() {
        if (!root.hasController
                || !root.controller.runtimeSmokeRequiresChatBubbleLayoutEvidence()
                || messageList.smokeBubbleLayoutReported) {
            return
        }

        const delegates = []
        for (let i = 0; i < messageList.contentItem.children.length; ++i) {
            const item = messageList.contentItem.children[i]
            if (item && item.objectName === "chatMessageDelegate" && item.visible) {
                delegates.push(item)
            }
        }
        if (delegates.length === 0 || messageList.count === 0) {
            return
        }

        delegates.sort(function(a, b) { return a.y - b.y })
        let stable = true
        let detail = "delegates=" + delegates.length + " count=" + messageList.count
        let previousBottom = -1
        for (let d = 0; d < delegates.length; ++d) {
            const delegateItem = delegates[d]
            let bubbleItem = null
            for (let c = 0; c < delegateItem.children.length; ++c) {
                if (delegateItem.children[c].objectName === "chatMessageBubble") {
                    bubbleItem = delegateItem.children[c]
                    break
                }
            }
            if (!bubbleItem || delegateItem.height <= 0 || bubbleItem.height <= 0 || bubbleItem.width <= 0) {
                stable = false
                detail += " invalid_size_at=" + d
                break
            }
            if (bubbleItem.x < -0.5 || bubbleItem.x + bubbleItem.width > delegateItem.width + 0.5
                    || bubbleItem.y < -0.5 || bubbleItem.y + bubbleItem.height > delegateItem.height + 0.5) {
                stable = false
                detail += " out_of_bounds_at=" + d
                break
            }
            if (previousBottom >= 0 && delegateItem.y < previousBottom - 0.5) {
                stable = false
                detail += " overlap_at=" + d
                break
            }
            previousBottom = delegateItem.y + delegateItem.height
        }

        messageList.smokeBubbleLayoutReported = true
        root.controller.reportChatBubbleLayoutEvidence(stable, detail)
    }

    Timer {
        id: bubbleLayoutSmokeTimer
        interval: 120
        repeat: false
        onTriggered: root.reportBubbleLayoutSmokeEvidence()
    }

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

        Controls.TextField {
            id: searchInput
            Layout.fillWidth: true
            placeholderText: "Search local chat"
            enabled: root.hasController
            onTextChanged: {
                if (!root.hasController || text.trim().length === 0) {
                    root.searchResults = []
                    return
                }
                root.searchResults = root.controller.searchLocalChat(text)
            }
        }

        ListView {
            id: searchList
            Layout.fillWidth: true
            Layout.preferredHeight: root.searchResults.length > 0 ? Math.min(96, contentHeight) : 0
            visible: root.searchResults.length > 0
            clip: true
            spacing: 6
            model: root.searchResults

            delegate: Controls.Label {
                required property var modelData
                width: searchList.width
                text: (modelData.senderName || modelData.senderId) + ": " + modelData.content
                color: "#cbd5e1"
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        ListView {
            id: messageList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 10
            model: root.hasController ? root.controller.chatMessageModel : null
            boundsBehavior: Flickable.DragOverBounds
            property real beforeOlderLoadContentHeight: 0
            property bool preservingOlderLoadPosition: false
            property bool smokeBubbleLayoutReported: false

            delegate: Item {
                id: delegateRoot
                objectName: "chatMessageDelegate"
                required property bool local
                required property string senderName
                required property string senderId
                required property string content
                required property var sentAt

                width: ListView.view.width
                readonly property real horizontalPadding: 12
                readonly property real maxBubbleWidth: Math.max(160, width * 0.86)
                readonly property real textWidth: Math.max(120, maxBubbleWidth - horizontalPadding * 2)
                height: Math.max(44, messageColumn.implicitHeight + horizontalPadding * 2)

                Rectangle {
                    id: bubble
                    objectName: "chatMessageBubble"
                    height: delegateRoot.height
                    width: Math.min(delegateRoot.maxBubbleWidth,
                                    Math.max(144, messageColumn.implicitWidth + delegateRoot.horizontalPadding * 2))
                    x: delegateRoot.local ? delegateRoot.width - width : 0
                    radius: 16
                    color: delegateRoot.local ? "#0f766e" : "#111827"
                    border.color: delegateRoot.local ? "#14b8a6" : "#1f2937"

                    ColumnLayout {
                        id: messageColumn
                        width: delegateRoot.textWidth
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: delegateRoot.horizontalPadding
                        spacing: 4

                        Controls.Label {
                            Layout.fillWidth: true
                            text: delegateRoot.senderName || delegateRoot.senderId
                            color: delegateRoot.local ? "#ccfbf1" : "#93c5fd"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }

                        Controls.Label {
                            id: messageText
                            Layout.fillWidth: true
                            text: delegateRoot.content
                            color: "#f8fafc"
                            font.pixelSize: 13
                            wrapMode: Text.WrapAnywhere
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            text: delegateRoot.sentAt > 0 ? Qt.formatTime(new Date(delegateRoot.sentAt), "HH:mm:ss") : ""
                            color: delegateRoot.local ? "#99f6e4" : "#64748b"
                            font.pixelSize: 10
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }

            header: Controls.Label {
                width: messageList.width
                height: visible ? 24 : 0
                visible: root.hasController && (root.controller.chatHistoryLoading || !root.controller.chatHistoryHasOlder)
                text: root.hasController && root.controller.chatHistoryLoading ? "Loading earlier messages..." : "No earlier messages"
                color: "#64748b"
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            onMovementEnded: {
                if (!root.hasController || root.controller.chatHistoryLoading || !root.controller.chatHistoryHasOlder) {
                    return
                }
                if (messageList.contentY <= -48 && messageList.count > 0) {
                    messageList.beforeOlderLoadContentHeight = messageList.contentHeight
                    messageList.preservingOlderLoadPosition = true
                    root.controller.loadOlderChatHistory()
                }
            }

            onContentHeightChanged: {
                if (!messageList.preservingOlderLoadPosition || messageList.beforeOlderLoadContentHeight <= 0) {
                    return
                }
                const delta = messageList.contentHeight - messageList.beforeOlderLoadContentHeight
                if (delta > 0) {
                    messageList.contentY += delta
                }
                messageList.preservingOlderLoadPosition = false
                messageList.beforeOlderLoadContentHeight = 0
                root.scheduleBubbleLayoutSmokeCheck()
            }

            onCountChanged: Qt.callLater(function() {
                if (messageList.count > 0 && !messageList.preservingOlderLoadPosition
                        && (!root.hasController || !root.controller.chatHistoryLoading)) {
                    messageList.positionViewAtEnd()
                }
                root.scheduleBubbleLayoutSmokeCheck()
            })

            Component.onCompleted: root.scheduleBubbleLayoutSmokeCheck()

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
