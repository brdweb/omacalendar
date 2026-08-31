pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

TextField {
    id: control
    property string accessibleName: placeholderText

    implicitHeight: 42
    leftPadding: 12
    rightPadding: 12
    color: Theme.text
    placeholderTextColor: Theme.mutedText
    selectionColor: Theme.accent
    selectedTextColor: Theme.darkBackground
    font.pixelSize: Theme.fontSize
    selectByMouse: true
    Accessible.name: accessibleName
    Accessible.role: Accessible.EditableText

    background: Rectangle {
        radius: Theme.smallRadius
        color: Theme.background
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus
                      ? Theme.accent
                      : Theme.alpha(Theme.text, 0.13)
    }
}
