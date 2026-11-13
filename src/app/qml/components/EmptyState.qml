pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import OmaCalendar

ColumnLayout {
    id: root

    property string iconText: "○"
    property string title: qsTr("Nothing here")
    property string description: ""
    property string actionText: ""
    // A quieter second button beside the action; empty hides it.
    property string secondaryActionText: ""
    signal actionRequested()
    signal secondaryActionRequested()

    width: 320
    spacing: 9

    Text {
        textFormat: Text.PlainText
        Layout.alignment: Qt.AlignHCenter
        text: root.iconText
        color: Theme.alpha(Theme.mutedText, 0.72)
        font.pixelSize: 30
    }
    Text {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        text: root.title
        color: Theme.text
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.fontSize + 2
        font.weight: Font.DemiBold
        wrapMode: Text.Wrap
    }
    Text {
        textFormat: Text.PlainText
        visible: text.length > 0
        Layout.fillWidth: true
        text: root.description
        color: Theme.mutedText
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.smallFontSize
        wrapMode: Text.Wrap
    }
    RowLayout {
        visible: root.actionText.length > 0 || root.secondaryActionText.length > 0
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: 3
        spacing: Theme.spacingSM
        AppButton {
            objectName: "emptyStateAction"
            visible: root.actionText.length > 0
            text: root.actionText
            primary: root.secondaryActionText.length > 0
            onClicked: root.actionRequested()
        }
        AppButton {
            objectName: "emptyStateSecondaryAction"
            visible: root.secondaryActionText.length > 0
            text: root.secondaryActionText
            quiet: true
            onClicked: root.secondaryActionRequested()
        }
    }
}
