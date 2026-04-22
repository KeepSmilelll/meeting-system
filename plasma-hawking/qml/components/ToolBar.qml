import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Controls.Pane {
    id: root
    required property var controller
    readonly property bool hasController: root.controller !== null && root.controller !== undefined
    readonly property var cameraDevices: root.hasController ? root.controller.availableCameraDevices : []
    readonly property var audioInputDevices: root.hasController ? root.controller.availableAudioInputDevices : []
    readonly property var audioOutputDevices: root.hasController ? root.controller.availableAudioOutputDevices : []
    readonly property bool hasCameraDevices: root.cameraDevices.length > 0
    readonly property bool hasAudioInputDevices: root.audioInputDevices.length > 0
    readonly property bool hasAudioOutputDevices: root.audioOutputDevices.length > 0

    implicitHeight: 82
    padding: 16

    background: Rectangle {
        radius: 20
        color: "#0b1220"
        border.color: "#1f2937"
    }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        Controls.Button {
            text: root.hasController && root.controller.audioMuted ? "Unmute mic" : "Mute mic"
            enabled: root.hasController && root.controller.inMeeting
            onClicked: {
                if (root.hasController) {
                    root.controller.toggleAudio()
                }
            }
        }

        Controls.Button {
            text: root.hasController && root.controller.videoMuted ? "Unmute camera" : "Mute camera"
            enabled: root.hasController && root.controller.inMeeting
            onClicked: {
                if (root.hasController) {
                    root.controller.toggleVideo()
                }
            }
        }

        Controls.Button {
            text: root.hasController && root.controller.screenSharing ? "Stop share" : "Share screen"
            enabled: root.hasController && root.controller.inMeeting
            onClicked: {
                if (root.hasController) {
                    root.controller.toggleScreenSharing()
                }
            }
        }

        Controls.Label {
            text: "Mic"
            color: root.hasAudioInputDevices ? "#cbd5e1" : "#64748b"
            font.pixelSize: 12
            Layout.alignment: Qt.AlignVCenter
        }

        Controls.ComboBox {
            id: micSelector
            Layout.preferredWidth: 180
            model: ["System default"].concat(root.audioInputDevices)
            enabled: root.hasAudioInputDevices

            function syncSelection() {
                if (!root.hasController || count <= 1) {
                    currentIndex = 0
                    return
                }
                const preferred = root.controller.preferredMicrophoneDevice
                const matchedIndex = preferred.length > 0 ? find(preferred) : -1
                currentIndex = matchedIndex >= 0 ? matchedIndex : 0
            }

            onActivated: {
                if (root.hasController && currentIndex >= 0) {
                    root.controller.setPreferredMicrophoneDevice(currentIndex === 0 ? "" : currentText)
                }
            }

            Component.onCompleted: syncSelection()
        }

        Controls.Label {
            text: "Speaker"
            color: root.hasAudioOutputDevices ? "#cbd5e1" : "#64748b"
            font.pixelSize: 12
            Layout.alignment: Qt.AlignVCenter
        }

        Controls.ComboBox {
            id: speakerSelector
            Layout.preferredWidth: 180
            model: ["System default"].concat(root.audioOutputDevices)
            enabled: root.hasAudioOutputDevices

            function syncSelection() {
                if (!root.hasController || count <= 1) {
                    currentIndex = 0
                    return
                }
                const preferred = root.controller.preferredSpeakerDevice
                const matchedIndex = preferred.length > 0 ? find(preferred) : -1
                currentIndex = matchedIndex >= 0 ? matchedIndex : 0
            }

            onActivated: {
                if (root.hasController && currentIndex >= 0) {
                    root.controller.setPreferredSpeakerDevice(currentIndex === 0 ? "" : currentText)
                }
            }

            Component.onCompleted: syncSelection()
        }

        Controls.Label {
            text: "Camera"
            color: root.hasCameraDevices ? "#cbd5e1" : "#64748b"
            font.pixelSize: 12
            Layout.alignment: Qt.AlignVCenter
        }

        Controls.ComboBox {
            id: cameraSelector
            Layout.preferredWidth: 180
            model: ["System default"].concat(root.cameraDevices)
            enabled: root.hasCameraDevices

            function syncSelection() {
                if (!root.hasController || count <= 1) {
                    currentIndex = 0
                    return
                }
                const preferred = root.controller.preferredCameraDevice
                const matchedIndex = preferred.length > 0 ? find(preferred) : -1
                currentIndex = matchedIndex >= 0 ? matchedIndex : 0
            }

            onActivated: {
                if (root.hasController && currentIndex >= 0) {
                    root.controller.setPreferredCameraDevice(currentIndex === 0 ? "" : currentText)
                }
            }

            Component.onCompleted: syncSelection()
        }

        Connections {
            target: root.hasController ? root.controller : null

            function onPreferredCameraDeviceChanged() {
                cameraSelector.syncSelection()
            }

            function onAvailableCameraDevicesChanged() {
                cameraSelector.syncSelection()
            }

            function onPreferredMicrophoneDeviceChanged() {
                micSelector.syncSelection()
            }

            function onPreferredSpeakerDeviceChanged() {
                speakerSelector.syncSelection()
            }

            function onAvailableAudioInputDevicesChanged() {
                micSelector.syncSelection()
            }

            function onAvailableAudioOutputDevicesChanged() {
                speakerSelector.syncSelection()
            }
        }

        Item {
            Layout.fillWidth: true
        }

        ColumnLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: 2

            Controls.Label {
                text: !root.hasController
                      ? "Closing meeting"
                      : (root.controller.meetingTitle !== ""
                          ? root.controller.meetingTitle + (root.controller.meetingId !== "" ? " (" + root.controller.meetingId + ")" : "")
                          : ((root.controller.meetingId === "") ? "No meeting selected" : "Meeting ID: " + root.controller.meetingId))
                color: "#e2e8f0"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignRight
            }

            Controls.Label {
                text: root.hasController ? root.controller.statusText : "Shutting down"
                color: "#94a3b8"
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
            }
        }

        Controls.Button {
            text: "Leave"
            enabled: root.hasController && root.controller.inMeeting
            onClicked: {
                if (root.hasController) {
                    root.controller.leaveMeeting()
                }
            }
        }
    }
}
