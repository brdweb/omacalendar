import QtQuick
import QtQuick.Controls
import OmaCalendar

ItemDelegate {
    id: root
    required property date date
    required property int month
    property bool selected: false
    property int eventCount: 0

    background: Rectangle {
        radius: 9
        color: root.selected
               ? Theme.alpha(Theme.accent, 0.18)
               : root.hovered ? Theme.alpha(Theme.text, 0.06) : "transparent"
        border.width: root.selected ? 1 : 0
        border.color: Theme.alpha(Theme.accent, 0.55)
    }
    contentItem: Column {
        anchors.centerIn: parent
        spacing: 4
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.date.getDate()
            color: root.month === root.date.getMonth()
                   ? Theme.text : Theme.alpha(Theme.mutedText, 0.45)
            font.pixelSize: Theme.smallFontSize
            font.weight: root.selected ? Font.Bold : Font.Normal
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 2
            Repeater {
                model: Math.min(3, root.eventCount)
                Rectangle {
                    width: 3
                    height: 3
                    radius: 2
                    color: Theme.accent
                }
            }
        }
    }
}

