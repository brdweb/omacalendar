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
    property string timeFormat: "system"
    property var activeOverflowPopup: null
    signal eventActivated(var eventData)
    signal dateSelected(date dateValue)
    signal createRequested(date dateValue)
    signal eventDateChanged(var eventData, date dateValue)

    readonly property date monthStart: new Date(currentDate.getFullYear(),
                                                currentDate.getMonth(), 1)
    readonly property date gridStart: firstGridDate(monthStart)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            spacing: 0

            Repeater {
                model: 7
                delegate: Text {
                    required property int index
                    Layout.fillWidth: true
                    text: Qt.formatDate(root.addDays(root.gridStart, index), "ddd").toUpperCase()
                    color: Theme.mutedText
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Theme.microFontSize
                    font.weight: Font.DemiBold
                }
            }
        }

        GridLayout {
            id: monthLayout
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 7
            rows: 6
            columnSpacing: 0
            rowSpacing: 0

            Repeater {
                model: 42
                delegate: Rectangle {
                    id: dayCell
                    required property int index
                    readonly property date dateValue: root.addDays(root.gridStart, index)
                    readonly property var dayEvents: root.eventsForDate(dateValue)
                    readonly property bool inMonth: dateValue.getMonth()
                                                    === root.monthStart.getMonth()
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 74
                    Layout.minimumHeight: 82
                    color: dayCellMouse.containsMouse
                           ? Theme.alpha(Theme.text, 0.035)
                           : root.sameDate(dateValue, root.currentDate)
                             ? Theme.alpha(Theme.accent, 0.028)
                             : "transparent"
                    border.width: 1
                    border.color: Theme.divider

                    MouseArea {
                        id: dayCellMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        onClicked: root.dateSelected(dayCell.dateValue)
                        onDoubleClicked: root.createRequested(dayCell.dateValue)
                    }

                    DropArea {
                        objectName: "monthDropArea-" + dayCell.index
                        anchors.fill: parent
                        keys: ["omacalendar-event"]
                        onDropped: drop => {
                            const draggedEvent = drop.source
                                    ? drop.source["eventData"] : null
                            if (draggedEvent
                                    && root.requestDateChange(draggedEvent,
                                                              dayCell.dateValue)) {
                                drop.acceptProposedAction()
                            }
                        }
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 5
                        spacing: 3

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 23
                            Rectangle {
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28
                                radius: 14
                                color: root.sameDate(dayCell.dateValue, new Date())
                                       ? Theme.accent : "transparent"
                                Text {
                                    anchors.fill: parent
                                    text: dayCell.dateValue.getDate()
                                    color: root.sameDate(dayCell.dateValue, new Date())
                                           ? Theme.accentText
                                           : dayCell.inMonth ? Theme.text
                                                             : Theme.alpha(Theme.mutedText, 0.5)
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: Theme.smallFontSize
                                    font.weight: root.sameDate(dayCell.dateValue, new Date())
                                                 ? Font.Bold : Font.Normal
                                }
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                visible: dayCell.dayEvents.length > 3
                                text: dayCell.dayEvents.length
                                color: Theme.mutedText
                                font.pixelSize: Theme.microFontSize
                            }
                        }

                        Repeater {
                            model: dayCell.dayEvents.slice(0, 3)
                            delegate: EventChip {
                                required property var modelData
                                objectName: "monthEvent-" + dayCell.index + "-"
                                            + String(modelData.id || "")
                                Layout.fillWidth: true
                                eventData: modelData
                                selected: root.eventReference(modelData)
                                          === root.selectedEventReference
                                compact: true
                                draggable: root.eventEditable(modelData)
                                showTime: !modelData.allDay
                                timeText: root.timeText(modelData, dayCell.dateValue)
                                onActivated: value => root.eventActivated(value)
                            }
                        }

                        Button {
                            id: overflowButton
                            objectName: "monthMore-" + dayCell.index
                            visible: dayCell.dayEvents.length > 3
                            Layout.fillWidth: true
                            text: "+" + (dayCell.dayEvents.length - 3) + qsTr(" more")
                            flat: true
                            padding: 0
                            Accessible.name: text + qsTr(" on ")
                                             + Qt.formatDate(dayCell.dateValue,
                                                             "MMMM d")
                            font.pixelSize: Theme.microFontSize
                            Keys.onReturnPressed: event => {
                                overflowButton.clicked()
                                event.accepted = true
                            }
                            Keys.onEnterPressed: event => {
                                overflowButton.clicked()
                                event.accepted = true
                            }
                            onClicked: {
                                root.activeOverflowPopup = overflowPopup
                                overflowPopup.open()
                            }
                            contentItem: Text {
                                text: overflowButton.text
                                color: overflowButton.hovered ? Theme.text
                                                              : Theme.mutedText
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                font: overflowButton.font
                            }
                            background: Rectangle {
                                radius: 4
                                color: overflowButton.hovered
                                       || overflowButton.activeFocus
                                       ? Theme.alpha(Theme.text, 0.07)
                                       : "transparent"
                                border.width: overflowButton.activeFocus ? 1 : 0
                                border.color: Theme.focus
                            }
                        }
                        Item { Layout.fillHeight: true }
                    }

                    Popup {
                        id: overflowPopup
                        objectName: "monthOverflow-" + dayCell.index
                        parent: Overlay.overlay
                        readonly property point cellOrigin: dayCell.mapToItem(
                                                                Overlay.overlay,
                                                                0,
                                                                dayCell.height)
                        x: Math.max(8, Math.min(cellOrigin.x,
                                               Overlay.overlay.width - width - 8))
                        y: Math.max(8, Math.min(cellOrigin.y,
                                               Overlay.overlay.height - height - 8))
                        width: Math.min(300, Overlay.overlay.width - 16)
                        height: Math.min(320, overflowColumn.implicitHeight + 16)
                        padding: 8
                        modal: false
                        dim: false
                        closePolicy: Popup.CloseOnEscape
                                     | Popup.CloseOnPressOutside
                        onClosed: {
                            if (root.activeOverflowPopup === overflowPopup)
                                root.activeOverflowPopup = null
                        }
                        background: Rectangle {
                            radius: Theme.radius
                            color: Theme.surfaceAlt
                            border.color: Theme.divider
                        }

                        contentItem: Flickable {
                            clip: true
                            contentWidth: width
                            contentHeight: overflowColumn.implicitHeight
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}

                            Column {
                                id: overflowColumn
                                width: parent.width
                                spacing: 4

                                Repeater {
                                    model: dayCell.dayEvents
                                    delegate: RowLayout {
                                        id: overflowEventRow
                                        required property var modelData
                                        width: overflowColumn.width
                                        spacing: 4

                                        AppButton {
                                            objectName: "monthOverflowPrevious-"
                                                        + dayCell.index + "-"
                                                        + String(
                                                            overflowEventRow.modelData.id
                                                            || "")
                                            Layout.preferredWidth: 28
                                            Layout.preferredHeight: 30
                                            iconText: "‹"
                                            compact: true
                                            quiet: true
                                            enabled: root.eventEditable(
                                                         overflowEventRow.modelData)
                                            Accessible.name: qsTr("Move ")
                                                + (overflowEventRow.modelData.summary
                                                   || qsTr("event"))
                                                + qsTr(" to previous day")
                                            onClicked: root.requestDateChange(
                                                           overflowEventRow.modelData,
                                                           root.addDays(
                                                               root.eventStart(
                                                                   overflowEventRow.modelData),
                                                               -1))
                                        }

                                        EventChip {
                                            objectName: "monthOverflowEvent-"
                                                        + dayCell.index + "-"
                                                        + String(
                                                            overflowEventRow.modelData.id
                                                            || "")
                                            Layout.fillWidth: true
                                            eventData: overflowEventRow.modelData
                                            selected: root.eventReference(
                                                          overflowEventRow.modelData)
                                                      === root.selectedEventReference
                                            compact: false
                                            draggable: root.eventEditable(
                                                           overflowEventRow.modelData)
                                            showTime: !overflowEventRow.modelData.allDay
                                            timeText: root.timeText(
                                                          overflowEventRow.modelData,
                                                          dayCell.dateValue)
                                            onActivated: value => {
                                                overflowPopup.close()
                                                root.eventActivated(value)
                                            }
                                        }

                                        AppButton {
                                            objectName: "monthOverflowNext-"
                                                        + dayCell.index + "-"
                                                        + String(
                                                            overflowEventRow.modelData.id
                                                            || "")
                                            Layout.preferredWidth: 28
                                            Layout.preferredHeight: 30
                                            iconText: "›"
                                            compact: true
                                            quiet: true
                                            enabled: root.eventEditable(
                                                         overflowEventRow.modelData)
                                            Accessible.name: qsTr("Move ")
                                                + (overflowEventRow.modelData.summary
                                                   || qsTr("event"))
                                                + qsTr(" to next day")
                                            onClicked: root.requestDateChange(
                                                           overflowEventRow.modelData,
                                                           root.addDays(
                                                               root.eventStart(
                                                                   overflowEventRow.modelData),
                                                               1))
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    function firstGridDate(dateValue) {
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(), 1)
        const jsFirstDay = firstDayOfWeek === 7 ? 0 : firstDayOfWeek
        const distance = (start.getDay() - jsFirstDay + 7) % 7
        return addDays(start, -distance)
    }

    function addDays(dateValue, days) {
        return new Date(dateValue.getFullYear(), dateValue.getMonth(),
                        dateValue.getDate() + days)
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
        const end = addDays(start, 1)
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

    function timeText(value, dateValue) {
        if (value.allDay)
            return ""
        const start = eventStart(value)
        if (!sameDate(start, dateValue))
            return "↳"
        return Qt.formatTime(start, timePattern())
    }

    function timePattern() {
        if (timeFormat === "24h")
            return "HH:mm"
        if (timeFormat === "12h")
            return "h:mm AP"
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

    function eventEditable(value) {
        const operationState = String(value.operationState || value.syncState || "")
        return value.readOnly !== true && value.conflict !== true
                && value.dirty !== true
                && operationState !== "pending" && operationState !== "sending"
                && operationState !== "blocked" && operationState !== "retry_wait"
                && operationState !== "failed" && operationState !== "error"
    }

    function requestDateChange(value, dateValue) {
        if (!eventEditable(value) || sameDate(eventStart(value), dateValue))
            return false
        eventDateChanged(value, dateValue)
        return true
    }
}
