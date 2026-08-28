import QtQuick
import QtQuick.Controls
import OmaCalendar

TextField {
    id: control

    implicitHeight: 42
    leftPadding: 12
    rightPadding: 12
    color: Theme.text
    placeholderTextColor: Theme.mutedText
    selectionColor: Theme.accent
    selectedTextColor: Theme.darkBackground
    font.pixelSize: Theme.fontSize
    selectByMouse: true

    background: Rectangle {
        radius: 9
        color: Theme.background
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus
                      ? Theme.accent
                      : Theme.alpha(Theme.text, 0.13)
    }
}
