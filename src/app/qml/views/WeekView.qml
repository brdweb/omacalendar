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
    property int firstDayOfWeek: 1
    property int firstHour: 0
    property int lastHour: 24
    property int workDayStart: 8
    property int workDayEnd: 18
    property int defaultDurationMinutes: 60
    property real pixelsPerHour: 62
    readonly property real rightGutter: 16
    property string timeFormat: "system"
    property bool headerDragActive: false
    signal eventActivated(var eventData)
    signal dateSelected(date dateValue)
    signal createRequested(date dateValue, int startMinute, int durationMinutes)
    signal eventTimeChanged(var eventData, date dateValue,
                            int startMinute, int durationMinutes)
    signal eventDateChanged(var eventData, date dateValue)

    readonly property date weekStart: startOfWeek(currentDate)

    Item {
        anchors.fill: parent

        RowLayout {
            id: dayHeaders
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.rightMargin: root.rightGutter
            anchors.top: parent.top
            height: 55
            spacing: 0

            Item { Layout.preferredWidth: 58 }
            Repeater {
                model: 7
                delegate: ItemDelegate {
                    id: dayHeader
                    objectName: "weekDayHeader-" + index
                    required property int index
                    readonly property date dateValue: root.addDays(root.weekStart, index)
                    Layout.preferredWidth: 0
                    Layout.minimumWidth: 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    padding: 0
                    onClicked: root.dateSelected(dateValue)
                    background: Rectangle {
                        color: dayHeader.hovered ? Theme.alpha(Theme.text, 0.045)
                                                 : "transparent"
                        border.color: Theme.divider
                    }
                    contentItem: Column {
                        anchors.centerIn: parent
                        spacing: 1
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: Qt.formatDate(dayHeader.dateValue, "ddd").toUpperCase()
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                            font.weight: Font.DemiBold
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: dayHeader.dateValue.getDate()
                            color: root.sameDate(dayHeader.dateValue, new Date())
                                   ? Theme.accentText : Theme.text
                            font.pixelSize: Theme.fontSize + 2
                            font.weight: Font.Bold
                            Rectangle {
                                visible: root.sameDate(dayHeader.dateValue, new Date())
                                anchors.centerIn: parent
                                width: 28
                                height: 28
                                radius: 14
                                color: Theme.accent
                                z: -1
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            id: headerLane
            objectName: "weekAllDayLane"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: dayHeaders.bottom
            height: 66
            color: Theme.darkBackground
            border.color: Theme.divider
            RowLayout {
                anchors.fill: parent
                anchors.rightMargin: root.rightGutter
                spacing: 0
                Text {
                    Layout.preferredWidth: 58
                    text: qsTr("all-day\nspanning")
                    color: Theme.mutedText
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Theme.microFontSize
                }
                Repeater {
                    model: 7
                    delegate: Item {
                        id: allDayColumn
                        objectName: "weekAllDayColumn-" + index
                        required property int index
                        readonly property date dateValue: root.addDays(root.weekStart, index)
                        readonly property var dayEvents: root.headerEventsForDate(dateValue)
                        Layout.preferredWidth: 0
                        Layout.minimumWidth: 0
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Rectangle {
                            anchors.fill: parent
                            color: "transparent"
                            border.color: Theme.divider
                        }
                        DropArea {
                            objectName: "weekHeaderDrop-" + allDayColumn.index
                            anchors.fill: parent
                            z: 1
                            keys: ["omacalendar-event"]
                            onDropped: drop => root.dropHeaderEvent(
                                                   drop, allDayColumn.dateValue)
                        }
                        Column {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 3
                            Repeater {
                                model: allDayColumn.dayEvents.slice(0, 2)
                                delegate: EventChip {
                                    required property var modelData
                                    objectName: "weekHeaderEvent-"
                                                + allDayColumn.index + "-"
                                                + String(modelData.id || "")
                                    width: parent.width
                                    eventData: modelData
                                    selected: root.eventReference(modelData)
                                              === root.selectedEventReference
                                    draggable: root.eventEditable(modelData)
                                    showTime: !modelData.allDay
                                    timeText: modelData.allDay ? "" : qsTr("multi-day")
                                    compact: true
                                    onActivated: value => root.eventActivated(value)
                                    onDragStarted: root.headerDragActive = true
                                    onDragFinished: root.headerDragActive = false
                                }
                            }
                            Text {
                                visible: allDayColumn.dayEvents.length > 2
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "+" + (allDayColumn.dayEvents.length - 2) + qsTr(" more")
                                color: Theme.mutedText
                                font.pixelSize: Theme.microFontSize
                            }
                        }
                    }
                }
            }

            DropArea {
                id: previousWeekDrop
                objectName: "weekPreviousDrop"
                enabled: root.headerDragActive
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 46
                z: 30
                keys: ["omacalendar-event"]
                onDropped: drop => {
                    root.dropHeaderEventByDays(drop, -7)
                    root.headerDragActive = false
                }
                Rectangle {
                    anchors.fill: parent
                    visible: previousWeekDrop.containsDrag
                    color: Theme.alpha(Theme.accent, 0.24)
                    border.color: Theme.accent
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("‹ week")
                        color: Theme.text
                        font.pixelSize: Theme.microFontSize
                        font.weight: Font.DemiBold
                        rotation: -90
                    }
                }
            }

            DropArea {
                id: nextWeekDrop
                objectName: "weekNextDrop"
                enabled: root.headerDragActive
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 46
                z: 30
                keys: ["omacalendar-event"]
                onDropped: drop => {
                    root.dropHeaderEventByDays(drop, 7)
                    root.headerDragActive = false
                }
                Rectangle {
                    anchors.fill: parent
                    visible: nextWeekDrop.containsDrag
                    color: Theme.alpha(Theme.accent, 0.24)
                    border.color: Theme.accent
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("week ›")
                        color: Theme.text
                        font.pixelSize: Theme.microFontSize
                        font.weight: Font.DemiBold
                        rotation: 90
                    }
                }
            }
        }

        Flickable {
            id: weekFlick
            objectName: "weekTimelineScroll"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: headerLane.bottom
            anchors.bottom: parent.bottom
            clip: true
            contentWidth: width
            contentHeight: (root.lastHour - root.firstHour) * root.pixelsPerHour
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            interactive: true
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AlwaysOn
                interactive: true
            }

            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: event => {
                    const steps = event.angleDelta.y / 120
                    weekFlick.contentY = Math.max(
                                0, Math.min(weekFlick.contentHeight
                                            - weekFlick.height,
                                            weekFlick.contentY
                                            - steps * root.pixelsPerHour))
                    event.accepted = true
                }
            }

            Component.onCompleted: contentY = Math.max(0, (root.workDayStart - 1)
                                                       * root.pixelsPerHour)

            Item {
                id: weekTimeline
                width: weekFlick.width
                height: weekFlick.contentHeight

                Repeater {
                    model: root.lastHour - root.firstHour + 1
                    delegate: Item {
                        id: hourMarker
                        required property int index
                        y: index * root.pixelsPerHour
                        width: weekTimeline.width
                        height: 1
                        Text {
                            width: 50
                            anchors.right: hourRule.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: hourRule.verticalCenter
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
                            id: hourRule
                            x: 58
                            width: parent.width - x - root.rightGutter
                            height: 1
                            color: Theme.divider
                        }
                    }
                }

                RowLayout {
                    x: 58
                    width: parent.width - 58 - root.rightGutter
                    height: parent.height
                    spacing: 0

                    Repeater {
                        model: 7
                        delegate: Item {
                            id: dayTimeline
                            objectName: "weekTimelineDay-" + index
                            required property int index
                            readonly property date dateValue: root.addDays(root.weekStart, index)
                            readonly property var dayEvents: root.eventsForDate(dateValue, false)
                            Layout.preferredWidth: 0
                            Layout.minimumWidth: 0
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Rectangle {
                                anchors.fill: parent
                                color: root.sameDate(dayTimeline.dateValue, new Date())
                                       ? Theme.alpha(Theme.accent, 0.028)
                                       : "transparent"
                                border.color: Theme.divider
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    y: (root.workDayStart - root.firstHour)
                                       * root.pixelsPerHour
                                    height: (root.workDayEnd - root.workDayStart)
                                            * root.pixelsPerHour
                                    color: Theme.alpha(Theme.accent, 0.02)
                                }
                            }

                            TimelineCanvas {
                                anchors.fill: parent
                                z: 1
                                firstHour: root.firstHour
                                lastHour: root.lastHour
                                pixelsPerHour: root.pixelsPerHour
                                defaultDurationMinutes: root.defaultDurationMinutes
                                onCreateRequested: (startMinute, durationMinutes) =>
                                                       root.createRequested(
                                                           dayTimeline.dateValue,
                                                           startMinute,
                                                           durationMinutes)
                            }

                            Repeater {
                                model: dayTimeline.dayEvents
                                delegate: TimelineEvent {
                                    required property var modelData
                                    objectName: "weekTimedEvent-"
                                                + dayTimeline.index + "-"
                                                + String(modelData.id || "")
                                    readonly property var overlapLayout:
                                        root.overlapLayout(dayTimeline.dayEvents,
                                                           modelData)
                                    readonly property int startValue: root.minuteOfDay(
                                                                              root.eventStart(modelData))
                                    readonly property int durationValue: Math.max(15,
                                        Math.round((root.eventEnd(modelData)
                                                    - root.eventStart(modelData)) / 60000))
                                    x: 3 + overlapLayout.column
                                       * ((dayTimeline.width - 6)
                                          / overlapLayout.columns)
                                    y: (startValue / 60 - root.firstHour)
                                       * root.pixelsPerHour
                                    width: Math.max(22,
                                                    (dayTimeline.width - 6)
                                                    / overlapLayout.columns - 3)
                                    height: Math.max(24, durationValue / 60
                                                     * root.pixelsPerHour - 2)
                                    z: interacting ? 100 : 2
                                    eventData: modelData
                                    editable: root.eventEditable(modelData)
                                    horizontalRescheduleEnabled: true
                                    dayWidth: dayTimeline.width
                                    selected: root.eventReference(modelData)
                                              === root.selectedEventReference
                                    startMinute: startValue
                                    durationMinutes: durationValue
                                    pixelsPerHour: root.pixelsPerHour
                                    onActivated: value => root.eventActivated(value)
                                    onRescheduleRequested: (value, startMinute,
                                                            durationMinutes,
                                                            dayOffset) => {
                                        root.eventTimeChanged(
                                                    value,
                                                    root.targetDateForMove(
                                                        dayTimeline.index,
                                                        dayOffset),
                                                    startMinute,
                                                    durationMinutes)
                                    }
                                }
                            }

                            Rectangle {
                                visible: root.sameDate(dayTimeline.dateValue, new Date())
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 2
                                y: root.minuteOfDay(new Date()) / 60
                                   * root.pixelsPerHour
                                color: Theme.danger
                                z: 20
                            }
                        }
                    }
                }
            }
        }
    }

    function startOfWeek(dateValue) {
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                               dateValue.getDate())
        const jsFirstDay = firstDayOfWeek === 7 ? 0 : firstDayOfWeek
        const distance = (start.getDay() - jsFirstDay + 7) % 7
        start.setDate(start.getDate() - distance)
        return start
    }

    function addDays(dateValue, days) {
        return new Date(dateValue.getFullYear(), dateValue.getMonth(),
                        dateValue.getDate() + days)
    }

    function targetDateForMove(sourceIndex, dayOffset) {
        return addDays(weekStart, sourceIndex + dayOffset)
    }

    function eventStart(value) {
        return value.allDay ? new Date(value.startDate + "T00:00:00")
                            : new Date(value.displayStartLocal || value.startUtc)
    }

    function eventEnd(value) {
        return value.allDay ? new Date(value.endDate + "T00:00:00")
                            : new Date(value.displayEndLocal || value.endUtc)
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

    function eventsForDate(dateValue, allDayValue) {
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                               dateValue.getDate())
        const end = addDays(start, 1)
        const matches = []
        for (let index = 0; index < events.length; ++index) {
            const value = events[index]
            const inHeader = value.allDay || spansCalendarDays(value)
            if (inHeader === allDayValue && eventStart(value) < end
                    && eventEnd(value) > start)
                matches.push(value)
        }
        matches.sort(function(first, second) {
            return eventStart(first) - eventStart(second)
        })
        return matches
    }

    function headerEventsForDate(dateValue) {
        return eventsForDate(dateValue, true)
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

    function dropHeaderEvent(drop, dateValue) {
        const draggedEvent = drop.source ? drop.source["eventData"] : null
        if (draggedEvent && requestDateChange(draggedEvent, dateValue))
            drop.acceptProposedAction()
    }

    function dropHeaderEventByDays(drop, days) {
        const draggedEvent = drop.source ? drop.source["eventData"] : null
        if (!draggedEvent)
            return
        const targetDate = addDays(eventStart(draggedEvent), days)
        if (requestDateChange(draggedEvent, targetDate))
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

    function eventReference(value) {
        return String(value.id || "") + "\n" + String(value.recurrenceId || "")
    }
}
