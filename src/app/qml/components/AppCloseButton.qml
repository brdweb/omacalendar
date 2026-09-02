pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

Button {
    id: control

    implicitWidth: 42
    implicitHeight: 42
    padding: 0
    hoverEnabled: true
    Accessible.name: toolTipText
    Accessible.role: Accessible.Button

    property string toolTipText: qsTr("Close")

    ToolTip.visible: hovered
    ToolTip.text: toolTipText
    ToolTip.delay: 450

    contentItem: Text {
        text: "×"
        color: control.down ? Theme.accentText : Theme.text
        font.pixelSize: 24
        font.weight: Font.Medium
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: width / 2
        color: control.down ? Theme.accent
                            : control.hovered ? Theme.accentSoft : Theme.surfaceAlt
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.focus : Theme.border
    }
}
