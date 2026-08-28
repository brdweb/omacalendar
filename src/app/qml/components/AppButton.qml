import QtQuick
import QtQuick.Controls
import OmaCalendar

Button {
    id: control
    property bool primary: false
    property bool quiet: false

    implicitHeight: 38
    implicitWidth: Math.max(88, contentItem.implicitWidth + 28)
    font.pixelSize: Theme.fontSize

    contentItem: Text {
        text: control.text
        color: control.primary ? Theme.darkBackground : Theme.text
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 9
        color: control.primary
               ? (control.down ? Qt.darker(Theme.accent, 1.18) : Theme.accent)
               : control.quiet
                 ? (control.hovered ? Theme.alpha(Theme.text, 0.08) : "transparent")
                 : (control.down ? Theme.surfaceAlt : Theme.surface)
        border.color: control.primary || control.quiet
                      ? "transparent" : Theme.alpha(Theme.text, 0.12)
    }
}

