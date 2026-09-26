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
    // Per-day counts are built once per events change. Each cell then does a
    // lookup instead of scanning every event, and a multi-day event counts on
    // every day it covers, matching the month and agenda views.
    readonly property var dayCounts: countEventsByDay(events,
                                                      currentDate.getFullYear())
    signal dateSelected(date dateValue)
    signal monthSelected(date dateValue)

    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical.policy: ScrollBar.AsNeeded

    GridLayout {
        width: root.availableWidth
        columns: root.width >= 1040 ? 4 : root.width >= 760 ? 3 : 2
        columnSpacing: Theme.spacingMD
        rowSpacing: Theme.spacingMD

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
                radius: Theme.radiusLG
                color: Theme.surface
                border.color: Theme.border
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMD
                    spacing: Theme.spacingXS

                    ItemDelegate {
                        id: monthHeader
                        Layout.fillWidth: true
                        implicitHeight: 28
                        padding: 0
                        onClicked: root.monthSelected(monthCard.monthDate)
                        background: Rectangle {
                            radius: Theme.radiusSM
                            color: monthHeader.hovered
                                   ? Theme.alpha(Theme.text, 0.055) : "transparent"
                        }
                        contentItem: Text {
                            textFormat: Text.PlainText
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
                            textFormat: Text.PlainText
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

    function eventStart(value) {
        return value.allDay ? new Date(value.startDate + "T00:00:00")
                            : new Date(value.displayStartLocal || value.startUtc)
    }

    function eventEnd(value) {
        return value.allDay ? new Date(value.endDate + "T00:00:00")
                            : new Date(value.displayEndLocal || value.endUtc)
    }

    function dayKey(dateValue) {
        return dateValue.getFullYear() * 10000 + (dateValue.getMonth() + 1) * 100
                + dateValue.getDate()
    }

    function countEventsByDay(values, year) {
        const counts = ({})
        // Month grids show a few days of the neighbouring months, so count a
        // margin around the year while bounding very long events.
        const windowStart = new Date(year - 1, 11, 1)
        const windowEnd = new Date(year + 1, 1, 1)
        for (let index = 0; index < values.length; ++index) {
            const start = eventStart(values[index])
            let end = eventEnd(values[index])
            if (isNaN(start.getTime()) || isNaN(end.getTime()))
                continue
            // End boundaries are exclusive; a zero-length event still marks
            // the day it starts on.
            if (end <= start)
                end = new Date(start.getTime() + 1)
            if (end <= windowStart || start >= windowEnd)
                continue
            const first = start > windowStart ? start : windowStart
            const last = end < windowEnd ? end : windowEnd
            let day = new Date(first.getFullYear(), first.getMonth(), first.getDate())
            while (day < last) {
                const key = dayKey(day)
                counts[key] = (counts[key] || 0) + 1
                day = new Date(day.getFullYear(), day.getMonth(), day.getDate() + 1)
            }
        }
        return counts
    }

    function eventCount(dateValue) {
        return dayCounts[dayKey(dateValue)] || 0
    }

    function sameDate(first, second) {
        return first && second
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }
}
