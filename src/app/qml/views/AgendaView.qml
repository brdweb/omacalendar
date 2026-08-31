pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../components"

Item {
    id: root

    property date currentDate: new Date()
    property var events: []
    property string selectedEventReference: ""
    property int dayCount: 31
    property string timeFormat: "system"
    signal eventActivated(var eventData)
    signal createRequested(date dateValue)
    signal dateSelected(date dateValue)

    ScrollView {
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: root.width
            spacing: 4

            Repeater {
                model: root.dayCount
                delegate: ColumnLayout {
                    id: daySection
                    required property int index
                    readonly property date dateValue: new Date(root.currentDate.getFullYear(),
                                                                root.currentDate.getMonth(),
                                                                root.currentDate.getDate() + index)
                    readonly property var dayEvents: root.eventsForDate(dateValue)
                    Layout.fillWidth: true
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: daySection.index === 0 ? 0 : 13
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            radius: 12
                            color: root.sameDate(daySection.dateValue, new Date())
                                   ? Theme.accent : Theme.surface
                            border.color: root.sameDate(daySection.dateValue, root.currentDate)
                                          ? Theme.focus : Theme.border
                            Column {
                                anchors.centerIn: parent
                                spacing: -2
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: Qt.formatDate(daySection.dateValue, "ddd").toUpperCase()
                                    color: root.sameDate(daySection.dateValue, new Date())
                                           ? Theme.accentText : Theme.mutedText
                                    font.pixelSize: Theme.microFontSize
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: daySection.dateValue.getDate()
                                    color: root.sameDate(daySection.dateValue, new Date())
                                           ? Theme.accentText : Theme.text
                                    font.pixelSize: Theme.fontSize + 2
                                    font.weight: Font.Bold
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.dateSelected(daySection.dateValue)
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                text: Qt.formatDate(daySection.dateValue, "dddd")
                                color: Theme.text
                                font.pixelSize: Theme.fontSize
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: Qt.formatDate(daySection.dateValue, "MMMM d")
                                color: Theme.mutedText
                                font.pixelSize: Theme.smallFontSize
                            }
                        }

                        AppButton {
                            iconText: "+"
                            quiet: true
                            compact: true
                            toolTipText: "New event on "
                                         + Qt.formatDate(daySection.dateValue, "MMMM d")
                            onClicked: root.createRequested(daySection.dateValue)
                        }
                    }

                    Repeater {
                        model: daySection.dayEvents
                        delegate: EventRow {
                            required property var modelData
                            Layout.fillWidth: true
                            eventData: modelData
                            selected: root.eventReference(modelData)
                                      === root.selectedEventReference
                            timeFormat: root.timeFormat
                            continuationText: root.continuationLabel(modelData,
                                                                     daySection.dateValue)
                            onEditRequested: value => root.eventActivated(value)
                        }
                    }

                    Rectangle {
                        visible: daySection.dayEvents.length === 0
                        Layout.fillWidth: true
                        implicitHeight: 44
                        radius: Theme.smallRadius
                        color: dayEmptyMouse.containsMouse
                               ? Theme.alpha(Theme.text, 0.045) : "transparent"
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 54
                            anchors.verticalCenter: parent.verticalCenter
                            text: "No events"
                            color: Theme.alpha(Theme.mutedText, 0.68)
                            font.pixelSize: Theme.smallFontSize
                        }
                        MouseArea {
                            id: dayEmptyMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onDoubleClicked: root.createRequested(daySection.dateValue)
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: 22 }
        }
    }

    function sameDate(first, second) {
        return first && second
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }

    function eventStart(value) {
        return value.allDay ? new Date(value.startDate + "T00:00:00")
                            : new Date(value.displayStartLocal || value.startUtc)
    }

    function eventEnd(value) {
        return value.allDay ? new Date(value.endDate + "T00:00:00")
                            : new Date(value.displayEndLocal || value.endUtc)
    }

    function eventsForDate(dateValue) {
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                               dateValue.getDate())
        const end = new Date(start.getFullYear(), start.getMonth(),
                             start.getDate() + 1)
        const matches = []
        for (let index = 0; index < events.length; ++index) {
            const value = events[index]
            if (eventStart(value) < end && eventEnd(value) > start)
                matches.push(value)
        }
        matches.sort(function(first, second) {
            if (first.allDay !== second.allDay)
                return first.allDay ? -1 : 1
            return eventStart(first) - eventStart(second)
        })
        return matches
    }

    function continuationLabel(value, dateValue) {
        const start = eventStart(value)
        const end = eventEnd(value)
        const dayStart = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                  dateValue.getDate())
        const dayEnd = new Date(dayStart.getFullYear(), dayStart.getMonth(),
                                dayStart.getDate() + 1)
        if (start < dayStart && end > dayEnd)
            return "Continues"
        if (start < dayStart)
            return "Ends today"
        if (end > dayEnd)
            return "Continues tomorrow"
        return ""
    }

    function eventReference(value) {
        return String(value.id || "") + "\n" + String(value.recurrenceId || "")
    }
}
