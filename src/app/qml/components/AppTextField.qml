pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

TextField {
    id: control
    // A visible or screen-reader label for the field. When set it names the
    // field, so an example placeholder ("https://…") is never read as the name.
    property string label: ""
    property string accessibleName: label.length > 0 ? label : placeholderText

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
    Accessible.description: accessibleName !== placeholderText ? placeholderText : ""
    Accessible.role: Accessible.EditableText

    background: Rectangle {
        radius: Theme.radiusMD
        color: Theme.background
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus
                      ? Theme.accent
                      : Theme.alpha(Theme.text, 0.13)
    }
}
