import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    required property Window window

    height: 40
    color: "#1f2937"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 8

        Text {
            text: "Meeting Client"
            color: "#f9fafb"
            font.pixelSize: 14
            Layout.fillWidth: true
            verticalAlignment: Text.AlignVCenter
        }

        Button {
            text: "-"
            onClicked: root.window.visibility = Window.Minimized
        }

        Button {
            text: "X"
            onClicked: root.window.close()
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onPressed: {
            if (root.window.startSystemMove) {
                root.window.startSystemMove()
            }
        }
    }
}
