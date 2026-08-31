pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import OmaCalendar

ColumnLayout {
    id: root

    property string iconText: "○"
    property string title: "Nothing here"
    property string description: ""
    property string actionText: ""
    signal actionRequested()

    width: 320
    spacing: 9

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.iconText
        color: Theme.alpha(Theme.mutedText, 0.72)
        font.pixelSize: 30
    }
    Text {
        Layout.fillWidth: true
        text: root.title
        color: Theme.text
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.fontSize + 2
        font.weight: Font.DemiBold
        wrapMode: Text.Wrap
    }
    Text {
        visible: text.length > 0
        Layout.fillWidth: true
        text: root.description
        color: Theme.mutedText
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.smallFontSize
        wrapMode: Text.Wrap
    }
    AppButton {
        visible: root.actionText.length > 0
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: 3
        text: root.actionText
        onClicked: root.actionRequested()
    }
}
