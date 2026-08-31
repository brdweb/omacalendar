pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../components"

ScrollView {
    id: root

    property date currentDate: new Date()
    property var events: []
    signal dateSelected(date dateValue)
    signal monthSelected(date dateValue)

    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical.policy: ScrollBar.AsNeeded

    GridLayout {
        width: root.availableWidth
        columns: root.width >= 1040 ? 4 : root.width >= 760 ? 3 : 2
        columnSpacing: 14
        rowSpacing: 14

        Repeater {
            model: 12
            delegate: Rectangle {
                id: monthCard
                objectName: "yearMonthCard-" + index
                required property int index
                readonly property date monthDate: new Date(root.currentDate.getFullYear(),
                                                            index, 1)
                Layout.fillWidth: true
                Layout.preferredHeight: 282
                radius: Theme.radius
                color: Theme.surface
                border.color: Theme.border
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 5

                    ItemDelegate {
                        id: monthHeader
                        Layout.fillWidth: true
                        implicitHeight: 28
                        padding: 0
                        onClicked: root.monthSelected(monthCard.monthDate)
                        background: Rectangle {
                            radius: 6
                            color: monthHeader.hovered
                                   ? Theme.alpha(Theme.text, 0.055) : "transparent"
                        }
                        contentItem: Text {
                            text: Qt.formatDate(monthCard.monthDate, "MMMM")
                            color: Theme.text
                            font.pixelSize: Theme.fontSize
                            font.weight: Font.Bold
                        }
                    }

                    DayOfWeekRow {
                        Layout.fillWidth: true
                        locale: Qt.locale()
                        delegate: Text {
                            required property string shortName
                            text: shortName.slice(0, 1)
                            color: Theme.mutedText
                            horizontalAlignment: Text.AlignHCenter
                            font.pixelSize: Theme.microFontSize
                        }
                    }

                    MonthGrid {
                        id: yearMonthGrid
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        month: monthCard.index
                        year: root.currentDate.getFullYear()
                        locale: Qt.locale()
                        delegate: MonthCell {
                            required property var model
                            date: model.date
                            month: yearMonthGrid.month
                            selected: root.sameDate(model.date, root.currentDate)
                            isToday: root.sameDate(model.date, new Date())
                            eventCount: root.eventCount(model.date)
                            onClicked: root.dateSelected(model.date)
                        }
                    }
                }
            }
        }
    }

    function eventDate(value) {
        return value.allDay ? new Date(value.startDate + "T00:00:00")
                            : new Date(value.displayStartLocal || value.startUtc)
    }

    function eventCount(dateValue) {
        let count = 0
        for (let index = 0; index < events.length; ++index) {
            if (sameDate(eventDate(events[index]), dateValue))
                ++count
        }
        return count
    }

    function sameDate(first, second) {
        return first && second
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }
}
