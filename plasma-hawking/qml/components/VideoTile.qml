import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import MeetingApp

Rectangle {
    id: root
    required property var controller
    required property string userId
    required property string displayName
    required property bool host
    required property bool audioOn
    required property bool videoOn
    required property bool sharing
    property int audioSsrc: 0
    property int videoSsrc: 0

    readonly property bool hasController: root.controller !== null && root.controller !== undefined
    readonly property bool localUser: root.hasController && userId === root.controller.userId
    readonly property bool remoteUser: userId !== "" && !localUser
    readonly property bool selected: root.hasController && root.controller.activeShareUserId === userId
    readonly property bool activeShareSelected: root.hasController && root.controller.hasActiveShare && selected
    readonly property bool activeRemoteVideoSelected: root.hasController
                                                      && !root.controller.hasActiveShare
                                                      && remoteUser
                                                      && (userId === root.controller.activeVideoPeerUserId)
    readonly property bool activeLocalCameraPreview: localUser && videoOn && !sharing
    readonly property bool remoteVideoAvailable: root.hasController
                                                 && !root.controller.hasActiveShare
                                                 && remoteUser
                                                 && videoOn
                                                 && root.remoteVideoSource() !== null
    readonly property bool showLiveFrame: activeShareSelected || remoteVideoAvailable || activeLocalCameraPreview
    readonly property bool shareFocusable: remoteUser && sharing && !selected
    property int frameSourceRevision: 0

    Connections {
        target: root.hasController ? root.controller : null
        function onRemoteVideoFrameSourceChanged() {
            root.frameSourceRevision += 1
        }
        function onLocalVideoFrameSourceChanged() {
            root.frameSourceRevision += 1
        }
    }

    function remoteVideoSource() {
        root.frameSourceRevision
        return root.hasController ? root.controller.remoteVideoFrameSourceForUser(root.userId) : null
    }

    implicitWidth: 320
    implicitHeight: 240

    radius: 18
    color: localUser ? "#132238" : "#111827"
    border.width: selected ? 2 : 1
    border.color: showLiveFrame ? "#22d3ee" : (selected ? "#38bdf8" : (host ? "#f59e0b" : "#1f2937"))
    clip: true

    MouseArea {
        anchors.fill: parent
        enabled: root.hasController
                 && remoteUser
                 && (sharing || videoOn || (userId === root.controller.activeVideoPeerUserId))
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: {
            if (!root.hasController) {
                return
            }
            if (sharing) {
                root.controller.setActiveShareUserId(userId)
            } else {
                root.controller.setActiveVideoPeerUserId(userId)
            }
        }
    }

    Item {
        anchors.fill: parent

        VideoRenderer {
            id: liveFrameRenderer
            anchors.fill: parent
            anchors.margins: 1
            frameSource: (!root.hasController || !root.showLiveFrame)
                        ? null
                        : (root.activeLocalCameraPreview
                        ? root.controller.localVideoFrameSource
                        : (root.activeShareSelected
                        ? root.controller.remoteScreenFrameSource
                        : root.remoteVideoSource()))
            visible: root.showLiveFrame
        }

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: root.showLiveFrame ? "#00000000" : "#18263d" }
                GradientStop { position: 1.0; color: root.showLiveFrame ? "#33000000" : "#0f172a" }
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Controls.Label {
                    text: root.displayName.trim().length > 0 ? root.displayName : root.userId
                    color: "#f8fafc"
                    font.pixelSize: 14
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Rectangle {
                    visible: root.host
                    radius: 999
                    color: "#f59e0b"
                    implicitWidth: 48
                    implicitHeight: 20
                    Controls.Label {
                        anchors.centerIn: parent
                        text: "Host"
                        color: "#fff7ed"
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    visible: root.localUser
                    radius: 999
                    color: "#22c55e"
                    implicitWidth: 44
                    implicitHeight: 20
                    Controls.Label {
                        anchors.centerIn: parent
                        text: "You"
                        color: "#f0fdf4"
                        font.pixelSize: 10
                    }
                }
            }

            Controls.Label {
                text: root.userId
                color: "#94a3b8"
                font.pixelSize: 11
                elide: Text.ElideRight
                visible: root.userId.length > 0
            }

            Controls.Label {
                text: root.shareFocusable ? "Click to focus shared screen" : ""
                color: "#67e8f9"
                font.pixelSize: 10
                visible: root.shareFocusable
                elide: Text.ElideRight
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    radius: 999
                    color: root.audioOn ? "#14532d" : "#7f1d1d"
                    implicitWidth: 72
                    implicitHeight: 20
                    Controls.Label {
                        anchors.centerIn: parent
                        text: root.audioOn ? "Mic on" : "Mic off"
                        color: "#f8fafc"
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    radius: 999
                    color: root.videoOn ? "#1e3a8a" : "#7f1d1d"
                    implicitWidth: 82
                    implicitHeight: 20
                    Controls.Label {
                        anchors.centerIn: parent
                        text: root.videoOn ? "Camera on" : "Camera off"
                        color: "#f8fafc"
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    visible: root.sharing
                    radius: 999
                    color: root.selected ? "#0c4a6e" : "#7c2d12"
                    implicitWidth: root.selected ? 84 : 76
                    implicitHeight: 20
                    Controls.Label {
                        anchors.centerIn: parent
                        text: root.selected ? "Watching" : "Sharing"
                        color: "#fff7ed"
                        font.pixelSize: 10
                    }
                }

                Item { Layout.fillWidth: true }
            }
        }
    }
}


