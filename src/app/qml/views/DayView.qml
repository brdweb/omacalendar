pragma ComponentBehavior: Bound
// DragEvent.source is typed as QObject; EventChip supplies eventData dynamically.
// qmllint disable missing-property
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
    property int firstHour: 0
    property int lastHour: 24
    property real pixelsPerHour: 68
    property int workDayStart: 8
    property int workDayEnd: 18
    property int defaultDurationMinutes: 60
    property string timeFormat: "system"
    property bool headerDragActive: false
    signal eventActivated(var eventData)
    signal createRequested(date dateValue, int startMinute, int durationMinutes)
    signal eventTimeChanged(var eventData, date dateValue,
                            int startMinute, int durationMinutes)
    signal eventDateChanged(var eventData, date dateValue)

    readonly property var allDayEvents: filterEvents(true)
    readonly property var spanningEvents: filterSpanningEvents()
    readonly property var headerEvents: allDayEvents.concat(spanningEvents)
    readonly property var timedEvents: filterEvents(false)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: headerLane
            visible: root.headerEvents.length > 0
            Layout.fillWidth: true
            implicitHeight: allDayColumn.implicitHeight + 14
            color: Theme.darkBackground
            border.color: Theme.divider

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.topMargin: 7
                anchors.bottomMargin: 7
                spacing: 10

                Text {
                    Layout.preferredWidth: 54
                    text: "all-day\nspanning"
                    color: Theme.mutedText
                    horizontalAlignment: Text.AlignRight
                    font.pixelSize: Theme.microFontSize
                }
                ColumnLayout {
                    id: allDayColumn
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: root.headerEvents
                        delegate: EventChip {
                            required property var modelData
                            objectName: "dayHeaderEvent-"
                                        + String(modelData.id || "")
                            Layout.fillWidth: true
                            eventData: modelData
                            selected: root.eventReference(modelData)
                                      === root.selectedEventReference
                            draggable: root.eventEditable(modelData)
                            showTime: !modelData.allDay
                            timeText: modelData.allDay ? "" : "multi-day"
                            compact: true
                            onActivated: value => root.eventActivated(value)
                            onDragStarted: root.headerDragActive = true
                            onDragFinished: root.headerDragActive = false
                        }
                    }
                }
            }

            DropArea {
                id: previousDayDrop
                objectName: "dayHeaderPreviousDrop"
                enabled: root.headerDragActive
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 68
                z: 30
                keys: ["omacalendar-event"]
                onDropped: drop => {
                    root.dropHeaderEvent(drop, root.addDays(root.currentDate, -1))
                    root.headerDragActive = false
                }
                Rectangle {
                    anchors.fill: parent
                    visible: previousDayDrop.containsDrag
                    color: Theme.alpha(Theme.accent, 0.24)
                    border.color: Theme.accent
                    Text {
                        anchors.centerIn: parent
                        text: "‹ day"
                        color: Theme.text
                        font.pixelSize: Theme.microFontSize
                        font.weight: Font.DemiBold
                    }
                }
            }

            DropArea {
                id: nextDayDrop
                objectName: "dayHeaderNextDrop"
                enabled: root.headerDragActive
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 68
                z: 30
                keys: ["omacalendar-event"]
                onDropped: drop => {
                    root.dropHeaderEvent(drop, root.addDays(root.currentDate, 1))
                    root.headerDragActive = false
                }
                Rectangle {
                    anchors.fill: parent
                    visible: nextDayDrop.containsDrag
                    color: Theme.alpha(Theme.accent, 0.24)
                    border.color: Theme.accent
                    Text {
                        anchors.centerIn: parent
                        text: "day ›"
                        color: Theme.text
                        font.pixelSize: Theme.microFontSize
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        Flickable {
            id: timelineFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: (root.lastHour - root.firstHour) * root.pixelsPerHour
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            Component.onCompleted: contentY = Math.max(0, (root.workDayStart - 1)
                                                       * root.pixelsPerHour)

            Item {
                id: timeline
                width: timelineFlick.width
                height: timelineFlick.contentHeight

                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: 68
                    anchors.right: parent.right
                    y: (root.workDayStart - root.firstHour) * root.pixelsPerHour
                    height: (root.workDayEnd - root.workDayStart) * root.pixelsPerHour
                    color: Theme.alpha(Theme.accent, 0.025)
                }

                Repeater {
                    model: root.lastHour - root.firstHour + 1
                    delegate: Item {
                        id: hourMarker
                        required property int index
                        x: 0
                        y: index * root.pixelsPerHour
                        width: timeline.width
                        height: 1
                        Text {
                            width: 55
                            anchors.right: hourLine.left
                            anchors.rightMargin: 10
                            anchors.verticalCenter: hourLine.verticalCenter
                            text: hourMarker.index === 0 ? ""
                                              : Qt.formatTime(new Date(2000, 0, 1,
                                                                       root.firstHour
                                                                       + hourMarker.index, 0),
                                                              root.hourPattern())
                            color: Theme.mutedText
                            horizontalAlignment: Text.AlignRight
                            font.pixelSize: Theme.microFontSize
                        }
                        Rectangle {
                            id: hourLine
                            x: 68
                            width: parent.width - x
                            height: 1
                            color: Theme.divider
                        }
                    }
                }

                TimelineCanvas {
                    anchors.left: parent.left
                    anchors.leftMargin: 68
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    z: 1
                    firstHour: root.firstHour
                    lastHour: root.lastHour
                    pixelsPerHour: root.pixelsPerHour
                    defaultDurationMinutes: root.defaultDurationMinutes
                    onCreateRequested: (startMinute, durationMinutes) =>
                                           root.createRequested(root.currentDate,
                                                                startMinute,
                                                                durationMinutes)
                }

                Repeater {
                    model: root.timedEvents
                    delegate: TimelineEvent {
                        required property var modelData
                        objectName: "dayTimedEvent-" + String(modelData.id || "")
                        readonly property var overlapLayout:
                            root.overlapLayout(root.timedEvents, modelData)
                        readonly property int startValue: root.minuteOfDay(
                                                                      root.eventStart(modelData))
                        readonly property int durationValue: Math.max(15,
                            Math.round((root.eventEnd(modelData)
                                        - root.eventStart(modelData)) / 60000))
                        x: 76 + overlapLayout.column
                           * ((timeline.width - 90) / overlapLayout.columns)
                        y: (startValue / 60 - root.firstHour) * root.pixelsPerHour
                        width: Math.max(42, (timeline.width - 90)
                                       / overlapLayout.columns - 4)
                        height: Math.max(24, durationValue / 60 * root.pixelsPerHour - 2)
                        z: interacting ? 100 : 2 + overlapLayout.column
                        eventData: modelData
                        editable: root.eventEditable(modelData)
                        selected: root.eventReference(modelData)
                                  === root.selectedEventReference
                        startMinute: startValue
                        durationMinutes: durationValue
                        pixelsPerHour: root.pixelsPerHour
                        onActivated: value => root.eventActivated(value)
                        onRescheduleRequested: (value, startMinute, durationMinutes,
                                                dayOffset) => {
                            root.eventTimeChanged(value,
                                                  root.addDays(root.currentDate,
                                                               dayOffset),
                                                  startMinute,
                                                  durationMinutes)
                        }
                    }
                }

                Rectangle {
                    visible: root.sameDate(root.currentDate, new Date())
                    x: 62
                    width: timeline.width - x
                    height: 2
                    y: root.minuteOfDay(new Date()) / 60 * root.pixelsPerHour
                    color: Theme.danger
                    z: 20
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 3
                        width: 8
                        height: 8
                        radius: 4
                        color: Theme.danger
                    }
                }
            }
        }
    }

    Timer {
        interval: 60000
        running: root.visible
        repeat: true
        onTriggered: timelineFlick.contentY = timelineFlick.contentY
    }

    function eventStart(value) {
        return value.allDay ? new Date(value.startDate + "T00:00:00")
                            : new Date(value.displayStartLocal || value.startUtc)
    }

    function eventEnd(value) {
        return value.allDay ? new Date(value.endDate + "T00:00:00")
                            : new Date(value.displayEndLocal || value.endUtc)
    }

    function filterEvents(allDayValue) {
        const start = new Date(currentDate.getFullYear(), currentDate.getMonth(),
                               currentDate.getDate())
        const end = new Date(start.getFullYear(), start.getMonth(), start.getDate() + 1)
        const matches = []
        for (let index = 0; index < events.length; ++index) {
            const value = events[index]
            if (value.allDay === allDayValue && eventStart(value) < end
                    && eventEnd(value) > start
                    && (allDayValue || !spansCalendarDays(value)))
                matches.push(value)
        }
        return matches
    }

    function filterSpanningEvents() {
        const start = new Date(currentDate.getFullYear(), currentDate.getMonth(),
                               currentDate.getDate())
        const end = addDays(start, 1)
        const matches = []
        for (let index = 0; index < events.length; ++index) {
            const value = events[index]
            if (!value.allDay && spansCalendarDays(value)
                    && eventStart(value) < end && eventEnd(value) > start) {
                matches.push(value)
            }
        }
        return matches
    }

    function spansCalendarDays(value) {
        if (value.allDay)
            return false
        const start = eventStart(value)
        const end = eventEnd(value)
        if (isNaN(start.getTime()) || isNaN(end.getTime()) || end <= start)
            return false
        return !sameDate(start, new Date(end.getTime() - 1))
    }

    function addDays(dateValue, days) {
        return new Date(dateValue.getFullYear(), dateValue.getMonth(),
                        dateValue.getDate() + days)
    }

    function dropHeaderEvent(drop, dateValue) {
        const draggedEvent = drop.source ? drop.source["eventData"] : null
        if (draggedEvent && requestDateChange(draggedEvent, dateValue))
            drop.acceptProposedAction()
    }

    function requestDateChange(value, dateValue) {
        if (!eventEditable(value) || sameDate(eventStart(value), dateValue))
            return false
        eventDateChanged(value, dateValue)
        return true
    }

    function minuteOfDay(dateValue) {
        return dateValue.getHours() * 60 + dateValue.getMinutes()
    }

    function snapMinute(value) {
        return Math.max(0, Math.min(1425, Math.round(value / 15) * 15))
    }

    function eventEditable(value) {
        const operationState = String(value.operationState || value.syncState || "")
        return value.readOnly !== true && value.conflict !== true
                && value.dirty !== true
                && operationState !== "pending" && operationState !== "sending"
                && operationState !== "blocked" && operationState !== "retry_wait"
                && operationState !== "failed" && operationState !== "error"
    }

    function hourPattern() {
        if (timeFormat === "24h")
            return "HH:mm"
        if (timeFormat === "12h")
            return "h AP"
        return Qt.locale().timeFormat(Locale.ShortFormat)
    }

    function sameDate(first, second) {
        return first && second
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }

    function overlapLayout(dayEvents, value) {
        const sorted = dayEvents.slice().sort(function(left, right) {
            const startDifference = eventStart(left) - eventStart(right)
            if (startDifference !== 0)
                return startDifference
            const endDifference = eventEnd(left) - eventEnd(right)
            if (endDifference !== 0)
                return endDifference
            return eventReference(left).localeCompare(eventReference(right))
        })
        const targetReference = eventReference(value)
        let group = []
        let groupEnd = null
        for (let index = 0; index <= sorted.length; ++index) {
            const candidate = index < sorted.length ? sorted[index] : null
            if (candidate && (group.length === 0
                              || eventStart(candidate) < groupEnd)) {
                group.push(candidate)
                if (!groupEnd || eventEnd(candidate) > groupEnd)
                    groupEnd = eventEnd(candidate)
                continue
            }
            if (group.length > 0) {
                const columnEnds = []
                const assignments = ({})
                for (let groupIndex = 0; groupIndex < group.length; ++groupIndex) {
                    const groupedEvent = group[groupIndex]
                    let column = 0
                    while (column < columnEnds.length
                           && columnEnds[column] > eventStart(groupedEvent))
                        ++column
                    columnEnds[column] = eventEnd(groupedEvent)
                    assignments[eventReference(groupedEvent)] = column
                }
                if (assignments[targetReference] !== undefined)
                    return {"column": assignments[targetReference],
                            "columns": Math.max(1, columnEnds.length)}
            }
            group = candidate ? [candidate] : []
            groupEnd = candidate ? eventEnd(candidate) : null
        }
        return {"column": 0, "columns": 1}
    }

    function eventReference(value) {
        return String(value.id || "") + "\n" + String(value.recurrenceId || "")
    }
}
