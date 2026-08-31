pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

Button {
    id: control
    property bool primary: false
    property bool quiet: false
    property bool destructive: false
    property bool compact: false
    property string iconText: ""
    property string toolTipText: ""

    implicitHeight: compact ? 32 : Theme.controlHeight
    implicitWidth: compact ? Math.max(34, contentRow.implicitWidth + 18)
                           : Math.max(84, contentRow.implicitWidth + 26)
    font.pixelSize: compact ? Theme.smallFontSize : Theme.fontSize
    hoverEnabled: true
    Accessible.name: text.length > 0 ? text : toolTipText
    Accessible.role: Accessible.Button

    ToolTip.visible: toolTipText.length > 0 && hovered
    ToolTip.text: toolTipText
    ToolTip.delay: 500

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight

        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: control.iconText.length > 0 && control.text.length > 0 ? 7 : 0
            Text {
                visible: control.iconText.length > 0
                text: control.iconText
                color: control.primary ? Theme.accentText
                                       : control.destructive ? Theme.danger : Theme.text
                font.pixelSize: control.font.pixelSize + 1
                verticalAlignment: Text.AlignVCenter
            }
            Text {
                visible: control.text.length > 0
                text: control.text
                color: control.primary ? Theme.accentText
                                       : control.destructive ? Theme.danger : Theme.text
                font: control.font
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
    }
    background: Rectangle {
        radius: Theme.smallRadius
        color: {
            if (!control.enabled)
                return control.quiet ? "transparent" : Theme.alpha(Theme.surfaceAlt, 0.55)
            if (control.primary)
                return control.down ? Qt.darker(Theme.accent, 1.18) : Theme.accent
            if (control.destructive && !control.quiet)
                return control.down ? Theme.alpha(Theme.danger, 0.25)
                                    : Theme.alpha(Theme.danger, 0.13)
            if (control.quiet)
                return control.hovered || control.activeFocus
                        ? Theme.alpha(Theme.text, 0.075) : "transparent"
            return control.down ? Theme.surfaceAlt : Theme.surface
        }
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.focus
                                           : control.primary || control.quiet
                                             ? "transparent" : Theme.border
        opacity: control.enabled ? 1 : 0.55
    }
}
