pragma ComponentBehavior: Bound
// DragEvent.source is typed as QObject; EventChip supplies eventData dynamically.
// qmllint disable missing-property
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../components"
import "../EventIndex.js" as EventIndex
import "../DateRange.js" as DateRange

Item {
    id: root

    property date currentDate: new Date()
    property var events: []
    // Built once per events change; day cells look their events up here.
    readonly property var eventIndex: EventIndex.build(events)
    property string selectedEventReference: ""
    property int firstDayOfWeek: 1
    property bool showWeekNumbers: false
    property int firstHour: 0
    property int lastHour: 24
    property int workDayStart: 8
    property int workDayEnd: 18
    property int defaultDurationMinutes: 60
    property real pixelsPerHour: 62
    readonly property real rightGutter: 16
    property string timeFormat: "system"
    // From App.secondaryTimeLabels: a second zone's time beside each hour.
    property var secondaryTime: ({})
    property bool headerDragActive: false
    // A remembered vertical position from an earlier visit; negative opens at
    // the current time when today is shown, otherwise before the work day.
    property real savedScrollY: -1
    signal scrollPositionChanged(real contentY)
    signal eventActivated(var eventData)
    signal dateSelected(date dateValue)
    signal createRequested(date dateValue, int startMinute, int durationMinutes)
    signal eventTimeChanged(var eventData, date dateValue,
                            int startMinute, int durationMinutes)
    signal eventDateChanged(var eventData, date dateValue)
    signal eventAllDayRequested(var eventData, date dateValue)
    // The all-day column a timed event is being dragged over, or -1.
    property int allDayHoverIndex: -1

    readonly property date weekStart: startOfWeek(currentDate)

    function initialScrollY() {
        if (savedScrollY >= 0)
            return savedScrollY
        const now = new Date()
        const shown = now >= weekStart && now < addDays(weekStart, 7)
        const hour = shown ? Math.max(workDayStart, now.getHours()) : workDayStart
        return Math.max(0, (hour - 1 - firstHour) * pixelsPerHour)
    }

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

            Text {
                objectName: "weekNumberLabel"
                Layout.preferredWidth: 58
                Layout.fillHeight: true
                textFormat: Text.PlainText
                text: root.showWeekNumbers
                      ? qsTr("W%1").arg(DateRange.rowWeekNumber(root.weekStart)) : ""
                color: Theme.mutedText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: Theme.smallFontSize
                font.weight: Font.DemiBold
                Accessible.name: root.showWeekNumbers
                                 ? qsTr("Week %1").arg(DateRange.rowWeekNumber(root.weekStart))
                                 : ""
            }
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
                            textFormat: Text.PlainText
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: Qt.formatDate(dayHeader.dateValue, "ddd").toUpperCase()
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                            font.weight: Font.DemiBold
                        }
                        Text {
                            textFormat: Text.PlainText
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
                                radius: Theme.radiusLG
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
            // A chip dragged out of the lane stays drawn above the timeline.
            z: root.headerDragActive ? 10 : 0
            color: Theme.darkBackground
            border.color: Theme.divider
            RowLayout {
                anchors.fill: parent
                anchors.rightMargin: root.rightGutter
                spacing: 0
                Text {
                    textFormat: Text.PlainText
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
                            id: headerDrop
                            objectName: "weekHeaderDrop-" + allDayColumn.index
                            anchors.fill: parent
                            z: 1
                            keys: ["omacalendar-event"]
                            onDropped: drop => root.dropHeaderEvent(
                                                   drop, allDayColumn.dateValue)
                        }
                        Rectangle {
                            objectName: "weekHeaderDropPreview-" + allDayColumn.index
                            anchors.fill: parent
                            anchors.margins: 1
                            visible: headerDrop.containsDrag
                                     || root.allDayHoverIndex === allDayColumn.index
                            color: Theme.alpha(Theme.accent, 0.16)
                            border.color: Theme.accent
                            border.width: 2
                            z: 2
                        }
                        Column {
                            anchors.fill: parent
                            anchors.margins: Theme.spacingXS
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
                                textFormat: Text.PlainText
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
                        textFormat: Text.PlainText
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
                        textFormat: Text.PlainText
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

            // The second zone's name stays in view above the gutter.
            Rectangle {
                parent: weekFlick
                visible: Boolean(root.secondaryTime.label)
                x: 0
                y: 0
                z: 30
                width: 58
                height: secondaryCaption.implicitHeight + 6
                color: Theme.background
                Text {
                    id: secondaryCaption
                    objectName: "secondaryZoneCaption"
                    anchors.centerIn: parent
                    width: parent.width - 4
                    textFormat: Text.PlainText
                    text: String(root.secondaryTime.label || "")
                    color: Theme.mutedText
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    font.pixelSize: Theme.microFontSize - 1
                    font.weight: Font.DemiBold
                    HoverHandler { id: captionHover }
                    ToolTip.visible: captionHover.hovered
                    ToolTip.text: String(root.secondaryTime.label || "") + " ("
                                  + String(root.secondaryTime.offsetLabel || "") + ")"
                }
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

            Component.onCompleted: contentY = root.initialScrollY()
            onContentYChanged: root.scrollPositionChanged(contentY)

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
                            id: primaryHourLabel
                            textFormat: Text.PlainText
                            width: 50
                            anchors.right: hourRule.left
                            anchors.rightMargin: Theme.spacingSM
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
                        Text {
                            objectName: "secondaryHourLabel-" + hourMarker.index
                            visible: text.length > 0
                            textFormat: Text.PlainText
                            width: 50
                            anchors.right: primaryHourLabel.right
                            anchors.top: primaryHourLabel.bottom
                            text: hourMarker.index === 0 ? ""
                                                       : root.secondaryHourText(root.firstHour + hourMarker.index)
                            color: Theme.alpha(Theme.mutedText, 0.72)
                            horizontalAlignment: Text.AlignRight
                            font.pixelSize: Theme.microFontSize - 1
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

                            DropArea {
                                id: timelineDrop
                                objectName: "weekTimelineDrop-" + dayTimeline.index
                                anchors.fill: parent
                                z: 3
                                keys: ["omacalendar-event"]
                                readonly property int previewMinute:
                                    root.snapMinute(drag.y / root.pixelsPerHour * 60
                                                    + root.firstHour * 60)
                                onDropped: drop => root.dropOnTimeline(
                                                   drop, dayTimeline.dateValue,
                                                   timelineDrop.previewMinute)
                                // Drop preview: where a chip from the all-day
                                // lane will start, at the default length.
                                Rectangle {
                                    objectName: "weekTimelineDropPreview-" + dayTimeline.index
                                    visible: timelineDrop.containsDrag
                                    x: 3
                                    width: parent.width - 6
                                    y: (timelineDrop.previewMinute / 60 - root.firstHour)
                                       * root.pixelsPerHour
                                    height: root.defaultDurationMinutes / 60 * root.pixelsPerHour
                                    radius: Theme.radiusSM
                                    color: Theme.alpha(Theme.accent, 0.2)
                                    border.color: Theme.accent
                                    border.width: 2
                                    // Bottom edge: the dragged chip covers the top.
                                    Text {
                                        anchors.left: parent.left
                                        anchors.bottom: parent.bottom
                                        anchors.margins: 5
                                        textFormat: Text.PlainText
                                        text: Qt.formatTime(new Date(2000, 0, 1,
                                                                     Math.floor(timelineDrop.previewMinute / 60),
                                                                     timelineDrop.previewMinute % 60),
                                                            root.hourPattern())
                                        color: Theme.text
                                        font.pixelSize: Theme.microFontSize
                                        font.weight: Font.DemiBold
                                    }
                                }
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
                                    allDayDropY: weekFlick.contentY
                                    onOverAllDayAreaChanged: root.allDayHoverIndex = overAllDayArea
                                        ? dayTimeline.index + snappedDayOffset : -1
                                    onSnappedDayOffsetChanged: {
                                        if (overAllDayArea)
                                            root.allDayHoverIndex = dayTimeline.index
                                                    + snappedDayOffset
                                    }
                                    onAllDayRequested: (value, dayOffset) => {
                                        root.allDayHoverIndex = -1
                                        if (root.eventEditable(value))
                                            root.eventAllDayRequested(
                                                        value,
                                                        root.targetDateForMove(
                                                            dayTimeline.index,
                                                            dayOffset))
                                    }
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
        return EventIndex.eventStart(value)
    }

    function eventEnd(value) {
        return EventIndex.eventEnd(value)
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
        const entries = EventIndex.entriesForDate(eventIndex, dateValue)
        const matches = []
        for (let index = 0; index < entries.length; ++index) {
            const value = entries[index].event
            const inHeader = value.allDay || spansCalendarDays(value)
            if (inHeader === allDayValue)
                matches.push(entries[index])
        }
        matches.sort(function(first, second) {
            return first.startMs - second.startMs
        })
        return matches.map(function(entry) { return entry.event })
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

    // A chip from the all-day lane dropped on a day's timeline becomes a
    // timed event there; all-day events take the default length.
    function dropOnTimeline(drop, dateValue, startMinute) {
        const draggedEvent = drop.source ? drop.source["eventData"] : null
        if (!draggedEvent || !eventEditable(draggedEvent))
            return
        const duration = draggedEvent.allDay
                ? defaultDurationMinutes
                : Math.max(15, Math.round((eventEnd(draggedEvent)
                                           - eventStart(draggedEvent)) / 60000))
        eventTimeChanged(draggedEvent, dateValue, startMinute, duration)
        drop.acceptProposedAction()
        headerDragActive = false
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

    // The second zone's time at display-zone hour, with +1 or -1 when it
    // falls on another day there.
    function secondaryHourText(hour) {
        const hours = secondaryTime.hours
        if (!hours || !hours[hour])
            return ""
        const value = hours[hour]
        // Compact enough for the gutter: no seconds, and "5:30am" not
        // "5:30 AM".
        const pattern = Theme.timePattern(timeFormat).replace(/:ss/, "")
                .replace(/\s*(AP|ap|A|a)$/, "ap")
        const text = Qt.formatTime(new Date(2000, 0, 1, Math.floor(value.minute / 60),
                                            value.minute % 60), pattern)
        return value.dayOffset > 0 ? text + " +1"
                                   : value.dayOffset < 0 ? text + " −1" : text
    }

    // Scrolls so minute (of the display-zone day) sits an hour below the top.
    function revealMinute(minute) {
        weekFlick.contentY = Math.max(0, Math.min(weekFlick.contentHeight - weekFlick.height,
                                                (minute / 60 - 1 - firstHour) * pixelsPerHour))
    }

    function hourPattern() {
        return Theme.hourPattern(timeFormat)
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

    Text {
        textFormat: Text.PlainText
        anchors.centerIn: parent
        visible: root.events.length === 0
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("No events this week")
              + "\n" + qsTr("Double-click a time slot or press Ctrl+N to create one")
        color: Theme.mutedText
        font.pixelSize: Theme.smallFontSize
        z: 10
    }
}
