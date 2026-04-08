import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import MeetingApp
import "../components"

Item {
    id: root
    required property var controller

    implicitWidth: 1280
    implicitHeight: 800

    readonly property bool wideLayout: width >= 1040

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
                        text: root.controller.meetingTitle !== ""
                                  ? root.controller.meetingTitle
                                  : ((root.controller.meetingId === "") ? "Meeting room" : "Meeting: " + root.controller.meetingId)
                        color: "#f8fafc"
                        font.pixelSize: 24
                        font.bold: true
                    }

                    Controls.Label {
                        text: root.controller.meetingId === "" ? root.controller.statusText : ("Meeting ID: " + root.controller.meetingId + "  |  " + root.controller.statusText)
                        color: "#94a3b8"
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    spacing: 6

                    Controls.Label {
                        text: root.controller.audioMuted ? "Mic muted" : "Mic live"
                        color: root.controller.audioMuted ? "#f87171" : "#22c55e"
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: 12
                    }

                    Controls.Label {
                        text: root.controller.videoMuted ? "Camera muted" : "Camera live"
                        color: root.controller.videoMuted ? "#f87171" : "#22c55e"
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: 12
                    }

                    Controls.Label {
                        text: root.controller.screenSharing ? "Screen sharing" : "Screen idle"
                        color: root.controller.screenSharing ? "#38bdf8" : "#94a3b8"
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
                            text: "Video stage"
                            color: "#e2e8f0"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Controls.Label {
                            text: root.controller.inMeeting ? "Media rendering will appear here." : "Create or join a meeting to enter the room."
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

                            Item {
                                anchors.fill: parent

                                VideoRenderer {
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    frameSource: root.controller.remoteScreenFrameSource
                                    visible: root.controller.inMeeting
                                }

                                Rectangle {
                                    anchors.fill: parent
                                    color: "#660b1220"
                                }

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 20
                                    spacing: 10

                                    Controls.Label {
                                        text: root.controller.hasActiveShare
                                                  ? ("Watching " + root.controller.activeShareDisplayName)
                                                  : (root.controller.inMeeting ? "Remote screen stage" : "Waiting for participants")
                                        color: "#f8fafc"
                                        font.pixelSize: 24
                                        font.bold: true
                                    }

                                    Controls.Label {
                                        text: root.controller.hasActiveShare
                                                  ? "Stage is locked to the selected remote sharer"
                                                  : (root.controller.screenSharing
                                                         ? "Local screen share sender active"
                                                         : "Renderer is waiting for remote screen packets")
                                        color: root.controller.hasActiveShare || root.controller.screenSharing ? "#38bdf8" : "#94a3b8"
                                        font.pixelSize: 13
                                    }

                                    Controls.Label {
                                        text: root.controller.audioMuted ? "Audio muted" : "Audio live"
                                        color: root.controller.audioMuted ? "#f87171" : "#22c55e"
                                        font.pixelSize: 13
                                    }

                                    Controls.Label {
                                        text: root.controller.participants.length > 0 ? "Participants in room: " + root.controller.participants.length : "No participants yet."
                                        color: "#cbd5e1"
                                        font.pixelSize: 12
                                    }

                                    Item { Layout.fillHeight: true }
                                }
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
                            text: "Video stage"
                            color: "#e2e8f0"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 18
                            color: "#111827"
                            border.color: "#1f2937"

                            Item {
                                anchors.fill: parent

                                VideoRenderer {
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    frameSource: root.controller.remoteScreenFrameSource
                                    visible: root.controller.inMeeting
                                }

                                Rectangle {
                                    anchors.fill: parent
                                    color: "#660b1220"
                                }

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 20
                                    spacing: 10

                                    Controls.Label {
                                        text: root.controller.hasActiveShare
                                                  ? ("Watching " + root.controller.activeShareDisplayName)
                                                  : (root.controller.inMeeting ? "Remote screen stage" : "Waiting for participants")
                                        color: "#f8fafc"
                                        font.pixelSize: 24
                                        font.bold: true
                                    }

                                    Controls.Label {
                                        text: root.controller.hasActiveShare
                                                  ? "Remote screen frames follow the selected sharer."
                                                  : (root.controller.inMeeting
                                                         ? "Remote screen frames render here when packets arrive."
                                                         : "Create or join a meeting to enter the room.")
                                        color: "#94a3b8"
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }

                                    Item { Layout.fillHeight: true }
                                }
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
