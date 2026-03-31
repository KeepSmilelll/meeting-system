import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Controls.Pane {
    id: root
    required property var controller

    implicitWidth: 320
    implicitHeight: 560
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
                text: "Participants"
                color: "#f8fafc"
                font.pixelSize: 18
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            Controls.Label {
                text: root.controller.participants.length
                color: "#38bdf8"
                font.pixelSize: 12
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            text: root.controller.participants.length > 0 ? "Everyone in the room is listed below." : "The room is empty right now."
            color: "#94a3b8"
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        ListView {
            id: participantList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 10
            model: root.controller.participantModel

            delegate: Rectangle {
                width: ListView.view.width
                height: 88
                radius: 16
                color: userId === root.controller.userId ? "#132238" : (index % 2 === 0 ? "#0f172a" : "#111827")
                border.color: host ? "#f59e0b" : "#1f2937"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Controls.Label {
                            text: displayName
                            color: "#f8fafc"
                            font.pixelSize: 14
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        Rectangle {
                            visible: host
                            radius: 999
                            color: "#f59e0b"
                            implicitWidth: 54
                            implicitHeight: 22

                            Controls.Label {
                                anchors.centerIn: parent
                                text: "Host"
                                color: "#fff7ed"
                                font.pixelSize: 11
                            }
                        }

                        Rectangle {
                            visible: userId === root.controller.userId
                            radius: 999
                            color: "#22c55e"
                            implicitWidth: 54
                            implicitHeight: 22

                            Controls.Label {
                                anchors.centerIn: parent
                                text: "You"
                                color: "#f0fdf4"
                                font.pixelSize: 11
                            }
                        }
                    }

                    Controls.Label {
                        text: userId
                        color: "#94a3b8"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            radius: 999
                            color: audioOn ? "#14532d" : "#7f1d1d"
                            implicitWidth: 78
                            implicitHeight: 22

                            Controls.Label {
                                anchors.centerIn: parent
                                text: audioOn ? "Mic on" : "Mic off"
                                color: "#f8fafc"
                                font.pixelSize: 11
                            }
                        }

                        Rectangle {
                            radius: 999
                            color: videoOn ? "#1e3a8a" : "#7f1d1d"
                            implicitWidth: 86
                            implicitHeight: 22

                            Controls.Label {
                                anchors.centerIn: parent
                                text: videoOn ? "Camera on" : "Camera off"
                                color: "#f8fafc"
                                font.pixelSize: 11
                            }
                        }

                        Rectangle {
                            visible: sharing
                            radius: 999
                            color: "#7c2d12"
                            implicitWidth: 86
                            implicitHeight: 22

                            Controls.Label {
                                anchors.centerIn: parent
                                text: "Sharing"
                                color: "#fff7ed"
                                font.pixelSize: 11
                            }
                        }

                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }
    }
}
