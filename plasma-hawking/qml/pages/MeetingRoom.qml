import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import MeetingApp
import "../components"

Item {
    id: root
    required property var controller
    readonly property bool hasController: root.controller !== null && root.controller !== undefined

    implicitWidth: 1280
    implicitHeight: 800

    readonly property bool wideLayout: width >= 1040

    function videoGridHint() {
        if (!root.hasController) {
            return "Meeting view is closing."
        }
        if (!root.controller.inMeeting) {
            return "Create or join a meeting to enter the room."
        }
        if (root.controller.hasActiveShare) {
            return "Focused share: " + root.controller.activeShareDisplayName + ". Tap a sharing tile to switch focus."
        }
        if (root.controller.participants.length <= 1) {
            return "Waiting for a remote participant. Your local tile stays visible."
        }
        return "1v1 and group tiles stay stable here. Tap a sharing tile to pin it."
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

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
                spacing: 16

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Controls.Label {
                        text: root.hasController && root.controller.meetingTitle !== ""
                                  ? root.controller.meetingTitle
                                  : ((root.hasController && root.controller.meetingId !== "") ? "Meeting: " + root.controller.meetingId : "Meeting room")
                        color: "#f8fafc"
                        font.pixelSize: 24
                        font.bold: true
                    }

                    Controls.Label {
                        text: !root.hasController
                              ? "Meeting window is closing."
                              : (root.controller.meetingId === ""
                                  ? root.controller.statusText
                                  : ("Meeting ID: " + root.controller.meetingId + "  |  " + root.controller.statusText))
                        color: "#94a3b8"
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    spacing: 6

                    Controls.Label {
                        text: root.hasController && root.controller.audioMuted ? "Mic muted" : "Mic live"
                        color: root.hasController && root.controller.audioMuted ? "#f87171" : "#22c55e"
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: 12
                    }

                    Controls.Label {
                        text: root.hasController && root.controller.videoMuted ? "Camera muted" : "Camera live"
                        color: root.hasController && root.controller.videoMuted ? "#f87171" : "#22c55e"
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: 12
                    }

                    Controls.Label {
                        text: root.hasController && root.controller.screenSharing ? "Screen sharing" : "Screen idle"
                        color: root.hasController && root.controller.screenSharing ? "#38bdf8" : "#94a3b8"
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: 12
                    }
                }
            }
        }

        Loader {
            id: layoutLoader
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: root.wideLayout ? wideLayoutComponent : narrowLayoutComponent
        }

        ToolBar {
            controller: root.controller
            Layout.fillWidth: true
        }
    }

    Component {
        id: wideLayoutComponent

        Item {
            anchors.fill: parent

            RowLayout {
                anchors.fill: parent
                spacing: 16

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
                            text: "Video grid"
                            color: "#e2e8f0"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            text: root.videoGridHint()
                            color: "#94a3b8"
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 18
                            color: "#111827"
                            border.color: "#1f2937"
                            VideoGrid {
                                anchors.fill: parent
                                anchors.margins: 12
                                controller: root.controller
                            }
                        }
                    }
                }

                ParticipantPanel {
                    Layout.preferredWidth: 340
                    Layout.fillHeight: true
                    controller: root.controller
                }
            }
        }
    }

    Component {
        id: narrowLayoutComponent

        Item {
            anchors.fill: parent

            ColumnLayout {
                anchors.fill: parent
                spacing: 16

                Controls.Pane {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 360
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
                            text: "Video grid"
                            color: "#e2e8f0"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            text: root.videoGridHint()
                            color: "#94a3b8"
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 18
                            color: "#111827"
                            border.color: "#1f2937"
                            VideoGrid {
                                anchors.fill: parent
                                anchors.margins: 12
                                controller: root.controller
                            }
                        }
                    }
                }

                ParticipantPanel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    controller: root.controller
                }
            }
        }
    }
}
