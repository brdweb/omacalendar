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
    // 0 to 4 shades the day by how busy it is instead of showing dots; -1
    // keeps the dots.
    property int heatLevel: -1

    Accessible.name: Qt.formatDate(date, "dddd, MMMM d")
                     + (eventCount > 0 ? ", " + qsTr("%n event(s)", "", eventCount) : "")

    background: Rectangle {
        radius: Theme.radiusSM
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
                radius: width / 2
                color: root.isToday ? Theme.accent
                                    : root.selected ? Theme.accentSoft
                                    : root.heatLevel > 0
                                      ? Theme.alpha(Theme.accent,
                                                    [0, 0.14, 0.26, 0.4, 0.58][root.heatLevel])
                                      : "transparent"
                border.width: root.selected && !root.isToday ? 1 : 0
                border.color: Theme.focus
                Text {
                    textFormat: Text.PlainText
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
                    model: root.heatLevel >= 0 ? 0 : Math.min(3, root.eventCount)
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
