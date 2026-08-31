pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

TabButton {
    id: control

    implicitHeight: 44
    font.pixelSize: Theme.fontSize
    font.weight: checked ? Font.DemiBold : Font.Normal
    hoverEnabled: true

    contentItem: Text {
        text: control.text
        color: control.checked ? Theme.text : Theme.mutedText
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: control.checked ? Theme.accentSoft
                               : control.hovered ? Theme.alpha(Theme.text, 0.05)
                                                 : "transparent"
        Rectangle {
            visible: control.checked
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: 2
            radius: 1
            color: Theme.accent
        }
    }
}
