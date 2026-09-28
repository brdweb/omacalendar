pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../components"
import "../EventIndex.js" as EventIndex

ScrollView {
    id: root

    property date currentDate: new Date()
    property var events: []
    // Built once per events change; each cell looks its count up, and a
    // multi-day event counts on every day it covers, as in the month view.
    readonly property var eventIndex: EventIndex.build(events)
    // Accent opacity for each heat level; MonthCell uses the same scale.
    readonly property var heatShades: [0, 0.14, 0.26, 0.4, 0.58]
    signal dateSelected(date dateValue)
    signal monthSelected(date dateValue)

    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical.policy: ScrollBar.AsNeeded

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.spacingSM

        RowLayout {
            objectName: "yearHeatLegend"
            Layout.alignment: Qt.AlignRight
            spacing: 4
            Text {
                textFormat: Text.PlainText
                text: qsTr("Fewer events")
                color: Theme.mutedText
                font.pixelSize: Theme.microFontSize
            }
            Repeater {
                model: root.heatShades.length
                delegate: Rectangle {
                    required property int index
                    width: 12
                    height: 12
                    radius: 6
                    color: Theme.alpha(Theme.accent, root.heatShades[index])
                    border.color: Theme.border
                    border.width: index === 0 ? 1 : 0
                }
            }
            Text {
                textFormat: Text.PlainText
                text: qsTr("More")
                color: Theme.mutedText
                font.pixelSize: Theme.microFontSize
            }
        }

        GridLayout {
            Layout.fillWidth: true
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
                                heatLevel: root.heatLevel(eventCount)
                                onClicked: root.dateSelected(model.date)
                            }
                        }
                    }
                }
            }
        }
    }

    // Fixed buckets keep a quiet year looking quiet instead of rescaling to
    // its busiest day.
    function heatLevel(count) {
        return count <= 0 ? 0 : count === 1 ? 1 : count <= 3 ? 2 : count <= 5 ? 3 : 4
    }

    function eventCount(dateValue) {
        return EventIndex.countForDate(eventIndex, dateValue)
    }

    function sameDate(first, second) {
        return first && second
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }
}
