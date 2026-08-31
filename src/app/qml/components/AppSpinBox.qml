pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

SpinBox {
    id: control

    implicitWidth: 96
    implicitHeight: Theme.controlHeight
    font.pixelSize: Theme.fontSize
    hoverEnabled: true

    contentItem: TextInput {
        z: 2
        text: control.textFromValue(control.value, control.locale)
        color: control.enabled ? Theme.text : Theme.alpha(Theme.mutedText, 0.65)
        selectionColor: Theme.accent
        selectedTextColor: Theme.darkBackground
        font: control.font
        horizontalAlignment: TextInput.AlignHCenter
        verticalAlignment: TextInput.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
    }

    up.indicator: Rectangle {
        x: control.width - width
        y: 1
        width: 30
        height: Math.floor((control.height - 2) / 2)
        color: control.up.pressed ? Theme.accentSoft
                                  : control.up.hovered ? Theme.surfaceAlt : "transparent"
        radius: Theme.smallRadius
        Text {
            anchors.centerIn: parent
            text: "⌃"
            color: control.enabled ? Theme.mutedText : Theme.alpha(Theme.mutedText, 0.5)
            font.pixelSize: Theme.smallFontSize
        }
    }

    down.indicator: Rectangle {
        x: control.width - width
        y: Math.ceil(control.height / 2)
        width: 30
        height: Math.floor((control.height - 2) / 2)
        color: control.down.pressed ? Theme.accentSoft
                                    : control.down.hovered ? Theme.surfaceAlt : "transparent"
        radius: Theme.smallRadius
        Text {
            anchors.centerIn: parent
            text: "⌄"
            color: control.enabled ? Theme.mutedText : Theme.alpha(Theme.mutedText, 0.5)
            font.pixelSize: Theme.smallFontSize
        }
    }

    background: Rectangle {
        radius: Theme.smallRadius
        color: Theme.surface
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.focus : Theme.border
    }
}
