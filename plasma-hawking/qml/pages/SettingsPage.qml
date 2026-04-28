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

    padding: 22

    background: Rectangle {
        radius: 24
        color: "#0b1220"
        border.color: "#1f2937"
    }

    function syncEndpoint() {
        if (!root.hasController) {
            return
        }
        serverHostField.text = root.controller.serverHost
        serverPortBox.value = root.controller.serverPort
        icePolicyBox.currentIndex = Math.max(0, icePolicyBox.find(root.controller.icePolicy))
        syncDeviceSelectors()
    }

    function syncDeviceSelectors() {
        if (!root.hasController) {
            return
        }
        micSelector.currentIndex = Math.max(0, micSelector.find(root.controller.preferredMicrophoneDevice))
        speakerSelector.currentIndex = Math.max(0, speakerSelector.find(root.controller.preferredSpeakerDevice))
        cameraSelector.currentIndex = Math.max(0, cameraSelector.find(root.controller.preferredCameraDevice))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 18

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            Controls.Label {
                text: "Settings"
                color: "#f8fafc"
                font.pixelSize: 24
                font.bold: true
            }

            Controls.Label {
                Layout.fillWidth: true
                text: "Configure the signaling endpoint, ICE policy, and preferred media devices. MEETING_ICE_POLICY still overrides this page during smoke runs."
                color: "#94a3b8"
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: root.width >= 760 ? 2 : 1
            columnSpacing: 18
            rowSpacing: 14

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Controls.Label { text: "Server host"; color: "#cbd5e1"; font.pixelSize: 12 }
                Controls.TextField {
                    id: serverHostField
                    Layout.fillWidth: true
                    placeholderText: "123.207.41.63"
                    enabled: root.hasController
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Controls.Label { text: "Server port"; color: "#cbd5e1"; font.pixelSize: 12 }
                Controls.SpinBox {
                    id: serverPortBox
                    Layout.fillWidth: true
                    from: 1
                    to: 65535
                    value: 8443
                    editable: true
                    enabled: root.hasController
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Controls.Label { text: "ICE policy"; color: "#cbd5e1"; font.pixelSize: 12 }
                Controls.ComboBox {
                    id: icePolicyBox
                    Layout.fillWidth: true
                    model: ["all", "relay-only"]
                    enabled: root.hasController
                    onActivated: {
                        if (root.hasController) {
                            root.controller.setIcePolicy(currentText)
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Controls.Label { text: "Camera"; color: "#cbd5e1"; font.pixelSize: 12 }
                Controls.ComboBox {
                    id: cameraSelector
                    Layout.fillWidth: true
                    model: ["System default"].concat(root.cameraDevices)
                    enabled: root.hasController
                    onActivated: {
                        if (root.hasController) {
                            root.controller.setPreferredCameraDevice(currentIndex === 0 ? "" : currentText)
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Controls.Label { text: "Microphone"; color: "#cbd5e1"; font.pixelSize: 12 }
                Controls.ComboBox {
                    id: micSelector
                    Layout.fillWidth: true
                    model: ["System default"].concat(root.audioInputDevices)
                    enabled: root.hasController
                    onActivated: {
                        if (root.hasController) {
                            root.controller.setPreferredMicrophoneDevice(currentIndex === 0 ? "" : currentText)
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Controls.Label { text: "Speaker"; color: "#cbd5e1"; font.pixelSize: 12 }
                Controls.ComboBox {
                    id: speakerSelector
                    Layout.fillWidth: true
                    model: ["System default"].concat(root.audioOutputDevices)
                    enabled: root.hasController
                    onActivated: {
                        if (root.hasController) {
                            root.controller.setPreferredSpeakerDevice(currentIndex === 0 ? "" : currentText)
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Controls.Button {
                text: "Save endpoint"
                enabled: root.hasController
                onClicked: root.controller.setServerEndpoint(serverHostField.text, serverPortBox.value)
            }

            Controls.Label {
                Layout.fillWidth: true
                text: root.hasController ? ("Current endpoint: " + root.controller.serverHost + ":" + root.controller.serverPort + " | ICE: " + root.controller.icePolicy) : ""
                color: "#94a3b8"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    Connections {
        target: root.hasController ? root.controller : null
        function onServerEndpointChanged() { root.syncEndpoint() }
        function onIcePolicyChanged() { root.syncEndpoint() }
        function onPreferredCameraDeviceChanged() { root.syncDeviceSelectors() }
        function onPreferredMicrophoneDeviceChanged() { root.syncDeviceSelectors() }
        function onPreferredSpeakerDeviceChanged() { root.syncDeviceSelectors() }
        function onAvailableCameraDevicesChanged() { root.syncDeviceSelectors() }
        function onAvailableAudioInputDevicesChanged() { root.syncDeviceSelectors() }
        function onAvailableAudioOutputDevicesChanged() { root.syncDeviceSelectors() }
    }

    Component.onCompleted: syncEndpoint()
}
