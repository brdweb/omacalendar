pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

CheckBox {
    id: control

    spacing: 9
    implicitHeight: 34
    font.pixelSize: Theme.fontSize
    hoverEnabled: true

    indicator: Rectangle {
        x: control.leftPadding
        y: Math.round((control.height - height) / 2)
        width: 19
        height: 19
        radius: 5
        color: control.checked ? Theme.accent
                               : control.hovered ? Theme.surfaceAlt : Theme.surface
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.focus
                                          : control.checked ? Theme.accent : Theme.border

        Text {
            anchors.centerIn: parent
            text: "✓"
            visible: control.checked
            color: Theme.accentText
            font.pixelSize: 13
            font.weight: Font.Bold
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        color: control.enabled ? Theme.text : Theme.alpha(Theme.mutedText, 0.65)
        font: control.font
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
}
