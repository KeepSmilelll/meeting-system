import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Item {
    id: root
    required property var controller
    readonly property bool hasController: root.controller !== null && root.controller !== undefined

    function computeColumns(count, availableWidth) {
        if (count <= 1) {
            return 1
        }
        if (count <= 2) {
            return availableWidth < 640 ? 1 : 2
        }
        if (count <= 4) {
            return availableWidth < 720 ? 1 : 2
        }
        if (count <= 6) {
            return availableWidth < 960 ? 2 : 3
        }
        return availableWidth < 1180 ? 3 : 4
    }

    readonly property int participantCount: participantRepeater.count
    readonly property bool hasParticipants: participantCount > 0
    readonly property bool oneOnOne: participantCount <= 2
    readonly property int columns: computeColumns(participantCount, width)
    readonly property int rows: hasParticipants ? Math.max(1, Math.ceil(participantCount / columns)) : 1
    readonly property real tileWidth: hasParticipants
                                     ? Math.max(oneOnOne ? 320 : 240, (width - (columns - 1) * grid.columnSpacing) / columns)
                                     : width
    readonly property real tileHeight: hasParticipants
                                      ? Math.max(oneOnOne ? 240 : 180, (height - (rows - 1) * grid.rowSpacing) / rows)
                                      : height
    readonly property string statusHint: !root.hasController
                                         ? "Meeting view is closing."
                                         : (!root.controller.inMeeting
                                         ? "Create or join a meeting to enter the room."
                                         : (root.controller.hasActiveShare
                                                ? ("Focused share: " + root.controller.activeShareDisplayName)
                                                : (participantCount <= 1
                                                       ? "Waiting for a remote participant. Your local tile stays visible."
                                                       : "Tap a sharing tile to focus the shared stream.")))

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Controls.Label {
            Layout.fillWidth: true
            text: root.statusHint
            color: "#94a3b8"
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            visible: (root.hasController && root.controller.inMeeting) || !root.hasParticipants
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Controls.Label {
                anchors.centerIn: parent
                text: (root.hasController && root.controller.inMeeting)
                      ? "No participants are available yet."
                      : "Enter a meeting to show video tiles."
                color: "#64748b"
                font.pixelSize: 12
                visible: !root.hasParticipants
            }

            GridLayout {
                id: grid
                anchors.fill: parent
                visible: root.hasParticipants
                columns: root.columns
                rowSpacing: 12
                columnSpacing: 12

                Repeater {
                    id: participantRepeater
                    model: root.hasController ? root.controller.participantModel : null

                    delegate: Item {
                        id: tileDelegate
                        required property string userId
                        required property string displayName
                        required property bool host
                        required property bool audioOn
                        required property bool videoOn
                        required property bool sharing
                        required property int audioSsrc
                        required property int videoSsrc

                        Layout.preferredWidth: root.tileWidth
                        Layout.preferredHeight: root.tileHeight
                        Layout.minimumWidth: root.tileWidth
                        Layout.minimumHeight: root.tileHeight
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        VideoTile {
                            anchors.fill: parent
                            controller: root.controller
                            userId: tileDelegate.userId
                            displayName: tileDelegate.displayName
                            host: tileDelegate.host
                            audioOn: tileDelegate.audioOn
                            videoOn: tileDelegate.videoOn
                            sharing: tileDelegate.sharing
                            audioSsrc: tileDelegate.audioSsrc
                            videoSsrc: tileDelegate.videoSsrc
                        }
                    }
                }
            }
        }
    }
}
