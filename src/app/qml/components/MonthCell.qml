pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

ItemDelegate {
    id: root
    required property date date
    required property int month
    property bool selected: false
    property bool isToday: false
    property int eventCount: 0

    background: Rectangle {
        radius: 7
        color: root.hovered ? Theme.alpha(Theme.text, 0.055) : "transparent"
    }
    contentItem: Item {
        Column {
            anchors.centerIn: parent
            spacing: 1
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 25
                height: 25
                radius: 13
                color: root.isToday ? Theme.accent
                                    : root.selected ? Theme.accentSoft : "transparent"
                border.width: root.selected && !root.isToday ? 1 : 0
                border.color: Theme.focus
                Text {
                    anchors.fill: parent
                    text: root.date.getDate()
                    color: root.isToday ? Theme.accentText
                                        : root.month === root.date.getMonth()
                                          ? Theme.text
                                          : Theme.alpha(Theme.mutedText, 0.42)
                    font.pixelSize: Theme.smallFontSize
                    font.weight: root.selected || root.isToday
                                 ? Font.Bold : Font.Normal
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                height: 4
                spacing: 2
                Repeater {
                    model: Math.min(3, root.eventCount)
                    Rectangle {
                        width: 4
                        height: 4
                        radius: 2
                        color: Theme.accent
                    }
                }
            }
        }
    }
}
