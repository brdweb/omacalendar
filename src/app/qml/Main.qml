pragma ComponentBehavior: Bound
// App and OmarchyTheme are intentionally supplied as context properties.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import OmaCalendar
import "components"
import "views"

ApplicationWindow {
    id: window

    width: 1440
    height: 900
    minimumWidth: 980
    minimumHeight: 660
    visible: true
    title: "OmaCalendar"
    color: Theme.background
    font.pixelSize: Theme.fontSize

    property string currentView: "month"
    property date visibleMonth: new Date(App.selectedDate.getFullYear(),
                                         App.selectedDate.getMonth(), 1)
    property var hiddenCalendars: ({})
    readonly property string activeCalendarSetId: String(
                                                     appValue("activeCalendarSetId",
                                                              "all-calendars"))
    property var localSearchResults: []
    property var activeSearchFilters: ({})
    property var selectedEvent: ({})
    readonly property string selectedEventReference: eventReference(selectedEvent)
    property bool sidebarVisible: true
    property bool firstLoadComplete: false
    property var pendingMoveEvent: ({})
    property var pendingMoveOptions: ({})
    property var pendingExportScope: ({})
    property string pendingGoogleDisplayName: ""

    readonly property var decoratedEvents: decorateEvents(App.events)
    readonly property var visibleEvents: filterVisibleEvents(decoratedEvents)
    readonly property var calendarSets: appList("calendarSets")
    readonly property var writableCalendars: appList("calendars").filter(
                                                 function(calendar) {
                                                     return calendar.enabled !== false
                                                             && !calendar.readOnly
                                                 })
    readonly property var localWritableCalendars: writableCalendars.filter(
                                                      function(calendar) {
                                                          return window.accountProvider(
                                                                      calendar.accountId)
                                                                  === "local"
                                                      })
    readonly property var invitations: appList("invitations")
    readonly property var conflicts: appList("conflicts")
    readonly property var operations: appList("operations")
    readonly property var searchResults: App.connected
                                         ? appList("searchResults")
                                         : localSearchResults
    readonly property var preferences: appObject("preferences")
    readonly property int configuredFirstDayOfWeek:
        preferences.firstDayOfWeek === undefined
        ? 0 : Number(preferences.firstDayOfWeek)
    readonly property int firstDayOfWeek:
        configuredFirstDayOfWeek === 0
        ? Number(Qt.locale().firstDayOfWeek) : configuredFirstDayOfWeek
    readonly property int currentViewIndex: viewIndex(currentView)

    header: Rectangle {
        implicitHeight: 68
        color: Theme.darkBackground
        border.color: Theme.divider

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 10

            AppButton {
                iconText: window.sidebarVisible ? "◧" : "▣"
                quiet: true
                compact: true
                toolTipText: window.sidebarVisible ? qsTr("Hide sidebar") : qsTr("Show sidebar")
                onClicked: window.sidebarVisible = !window.sidebarVisible
            }

            Row {
                spacing: 4
                Text {
                    text: "oma"
                    color: Theme.mutedText
                    font.pixelSize: Theme.fontSize + 4
                    font.weight: Font.Light
                }
                Text {
                    text: "calendar"
                    color: Theme.text
                    font.pixelSize: Theme.fontSize + 4
                    font.weight: Font.Bold
                }
            }

            Rectangle {
                Layout.leftMargin: 6
                Layout.preferredWidth: 1
                Layout.preferredHeight: 28
                color: Theme.divider
            }

            AppButton {
                iconText: "‹"
                quiet: true
                compact: true
                toolTipText: qsTr("Previous period  [")
                onClicked: window.navigatePeriod(-1)
            }
            AppButton {
                text: qsTr("Today")
                quiet: true
                compact: true
                onClicked: window.goToday()
            }
            AppButton {
                iconText: "›"
                quiet: true
                compact: true
                toolTipText: qsTr("Next period  ]")
                onClicked: window.navigatePeriod(1)
            }

            ColumnLayout {
                Layout.preferredWidth: 240
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: window.periodTitle()
                    color: Theme.text
                    font.pixelSize: Theme.fontSize + 2
                    font.weight: Font.Bold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: window.periodSubtitle()
                    color: Theme.mutedText
                    font.pixelSize: Theme.microFontSize
                    elide: Text.ElideRight
                }
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                Layout.preferredWidth: viewSwitch.implicitWidth + 8
                Layout.preferredHeight: 40
                radius: 10
                color: Theme.background
                border.color: Theme.border
                RowLayout {
                    id: viewSwitch
                    anchors.centerIn: parent
                    spacing: 2
                    Repeater {
                        model: [
                            {"id": "agenda", "label": qsTr("Agenda"), "key": "1"},
                            {"id": "day", "label": qsTr("Day"), "key": "2"},
                            {"id": "week", "label": qsTr("Week"), "key": "3"},
                            {"id": "month", "label": qsTr("Month"), "key": "4"},
                            {"id": "year", "label": qsTr("Year"), "key": "5"}
                        ]
                        delegate: AppButton {
                            required property var modelData
                            text: modelData.label
                            compact: true
                            quiet: window.currentView !== modelData.id
                            primary: window.currentView === modelData.id
                            toolTipText: modelData.label + qsTr(" view  Alt+") + modelData.key
                            onClicked: window.setView(modelData.id)
                        }
                    }
                }
            }

            AppButton {
                iconText: "⌕"
                quiet: true
                compact: true
                toolTipText: qsTr("Search  Ctrl+F")
                onClicked: window.openActivity("search")
            }

            StatusBadge {
                text: App.connected ? (App.busy ? qsTr("Syncing") : qsTr("Connected")) : qsTr("Offline")
                tone: App.connected ? (App.busy ? "info" : "success") : "danger"
            }

            AppButton {
                text: qsTr("New event")
                iconText: "+"
                primary: true
                onClicked: editor.openNew(App.selectedDate, 540)
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        CalendarSidebar {
            visible: window.sidebarVisible
            Layout.preferredWidth: visible ? Theme.sidebarWidth : 0
            Layout.fillHeight: true
            currentDate: App.selectedDate
            monthDate: window.visibleMonth
            calendars: App.calendars
            calendarSets: window.calendarSets
            calendarsModel: window.appValue("calendarsModel", null)
            calendarSetsModel: window.appValue("calendarSetsModel", null)
            activeSetId: window.activeCalendarSetId
            connected: App.connected
            invitationCount: window.invitations.length
            conflictCount: window.conflicts.length
            failedOperationCount: window.failedOperationCount()
            eventCountForDate: function(dateValue) {
                return window.eventsForDate(dateValue, window.visibleEvents).length
            }
            calendarIsVisible: function(calendarId) {
                return window.calendarIsVisible(calendarId)
            }
            onDateSelected: dateValue => {
                window.selectDate(dateValue)
                if (window.currentView === "year")
                    window.setView("day")
            }
            onMonthChanged: dateValue => {
                window.visibleMonth = dateValue
                window.loadRangeFor(dateValue, "month")
            }
            onSetActivated: setId => window.activateCalendarSet(setId)
            onCalendarVisibilityRequested: (calendarId, visible) =>
                                               window.setCalendarVisible(calendarId, visible)
            onPanelRequested: panelName => window.openActivity(panelName)
            onSettingsRequested: settingsDrawer.open()
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.background

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    visible: App.lastError.length > 0
                    Layout.fillWidth: true
                    implicitHeight: errorRow.implicitHeight + 18
                    color: Theme.alpha(Theme.danger, 0.1)
                    border.color: Theme.alpha(Theme.danger, 0.3)
                    RowLayout {
                        id: errorRow
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 10
                        anchors.topMargin: 9
                        anchors.bottomMargin: 9
                        spacing: 10
                        StatusBadge { dotOnly: true; text: qsTr("Error"); tone: "danger" }
                        Text {
                            Layout.fillWidth: true
                            text: App.lastError
                            color: Theme.text
                            font.pixelSize: Theme.smallFontSize
                            wrapMode: Text.Wrap
                        }
                        AppButton {
                            visible: !App.connected
                            text: qsTr("Reconnect")
                            compact: true
                            onClicked: App.reconnect()
                        }
                        AppButton {
                            text: qsTr("Details")
                            compact: true
                            quiet: true
                            onClicked: window.openActivity("sync")
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    StackLayout {
                        id: viewStack
                        anchors.fill: parent
                        anchors.leftMargin: window.currentView === "month"
                                            || window.currentView === "week" ? 0 : 22
                        anchors.rightMargin: anchors.leftMargin
                        anchors.topMargin: window.currentView === "month"
                                           || window.currentView === "week" ? 0 : 18
                        anchors.bottomMargin: anchors.topMargin
                        currentIndex: Math.max(0, window.currentViewIndex)

                        AgendaView {
                            currentDate: App.selectedDate
                            events: window.visibleEvents
                            selectedEventReference: window.selectedEventReference
                            timeFormat: String(window.preferences.timeFormat || "system")
                            onEventActivated: value => window.openEvent(value)
                            onCreateRequested: dateValue => editor.openNew(dateValue, 540)
                            onDateSelected: dateValue => window.selectDate(dateValue)
                        }

                        DayView {
                            currentDate: App.selectedDate
                            events: window.visibleEvents
                            selectedEventReference: window.selectedEventReference
                            workDayStart: Number(window.preferences.workDayStart || 8)
                            workDayEnd: Number(window.preferences.workDayEnd || 18)
                            defaultDurationMinutes:
                                Number(window.preferences.defaultDuration || 60)
                            timeFormat: String(window.preferences.timeFormat || "system")
                            onEventActivated: value => window.openEvent(value)
                            onCreateRequested: (dateValue, startMinute,
                                                durationMinutes) =>
                                                   editor.openNew(dateValue, startMinute,
                                                                  durationMinutes)
                            onEventTimeChanged: (value, dateValue, startMinute, durationMinutes) =>
                                                    window.rescheduleEvent(value, dateValue,
                                                                           startMinute,
                                                                           durationMinutes)
                            onEventDateChanged: (value, dateValue) =>
                                                    window.moveEventToDate(value,
                                                                           dateValue)
                        }

                        WeekView {
                            currentDate: App.selectedDate
                            events: window.visibleEvents
                            selectedEventReference: window.selectedEventReference
                            firstDayOfWeek: window.firstDayOfWeek
                            workDayStart: Number(window.preferences.workDayStart || 8)
                            workDayEnd: Number(window.preferences.workDayEnd || 18)
                            defaultDurationMinutes:
                                Number(window.preferences.defaultDuration || 60)
                            timeFormat: String(window.preferences.timeFormat || "system")
                            onEventActivated: value => window.openEvent(value)
                            onDateSelected: dateValue => {
                                window.selectDate(dateValue)
                                window.setView("day")
                            }
                            onCreateRequested: (dateValue, startMinute,
                                                durationMinutes) =>
                                                   editor.openNew(dateValue, startMinute,
                                                                  durationMinutes)
                            onEventTimeChanged: (value, dateValue, startMinute, durationMinutes) =>
                                                    window.rescheduleEvent(value, dateValue,
                                                                           startMinute,
                                                                           durationMinutes)
                            onEventDateChanged: (value, dateValue) =>
                                                    window.moveEventToDate(value,
                                                                           dateValue)
                        }

                        MonthView {
                            currentDate: App.selectedDate
                            events: window.visibleEvents
                            selectedEventReference: window.selectedEventReference
                            firstDayOfWeek: window.firstDayOfWeek
                            timeFormat: String(window.preferences.timeFormat || "system")
                            onEventActivated: value => window.openEvent(value)
                            onDateSelected: dateValue => window.selectDate(dateValue)
                            onCreateRequested: dateValue => editor.openNew(dateValue, 540)
                            onEventDateChanged: (value, dateValue) =>
                                                    window.moveEventToDate(value, dateValue)
                        }

                        YearView {
                            currentDate: App.selectedDate
                            events: window.visibleEvents
                            onDateSelected: dateValue => {
                                window.selectDate(dateValue)
                                window.setView("day")
                            }
                            onMonthSelected: dateValue => {
                                window.selectDate(dateValue)
                                window.setView("month")
                            }
                        }
                    }

                    EmptyState {
                        visible: App.calendars.length === 0 && !App.busy
                        anchors.centerIn: parent
                        width: Math.min(420, parent.width - 60)
                        iconText: "◫"
                        title: qsTr("Your calendar, locally first")
                        description: qsTr("Create a device-only calendar or connect Google, CalDAV, or an ICS subscription.")
                        actionText: qsTr("Add a calendar")
                        onActionRequested: settingsDrawer.open()
                    }

                    Rectangle {
                        visible: App.busy && !window.firstLoadComplete
                        anchors.fill: parent
                        color: Theme.alpha(Theme.background, 0.78)
                        BusyIndicator {
                            anchors.centerIn: parent
                            running: true
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.darkBackground
                    border.color: Theme.divider
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 9
                        Text {
                            Layout.fillWidth: true
                            text: App.busy ? qsTr("Working…") : App.statusText
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                            elide: Text.ElideRight
                        }
                        Text {
                            text: window.visibleEvents.length + qsTr(" events loaded")
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                        }
                        Rectangle {
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: 12
                            color: Theme.divider
                        }
                        Text {
                            text: OmarchyTheme.sourceName
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                        }
                    }
                }
            }
        }
    }

    EventEditor {
        id: editor
        defaultDurationMinutes: Number(window.preferences.defaultDuration || 60)
        defaultCalendarId: String(window.preferences.defaultCalendarId || "")
        onSaveRequested: (value, options) => window.saveEvent(value, options)
        onRemoveRequested: (eventId, options) => window.removeEvent(eventId, options)
        onDuplicateRequested: value => window.duplicateEvent(value)
        onExportRequested: eventId => window.beginEventExport(eventId)
        onJoinRequested: url => Qt.openUrlExternally(url)
    }

    MutationConfirmationDialog {
        id: mutationConfirmation
        onConfirmed: (context, options) => {
            if (context.kind === "delete") {
                window.removeEvent(String(context.eventId || ""), options)
                return
            }
            if (context.kind === "save")
                window.performInteractionMutation(context.draft || ({}), options)
        }
    }

    Dialog {
        id: crossAccountMoveDialog
        anchors.centerIn: Overlay.overlay
        width: Math.min(470, Overlay.overlay ? Overlay.overlay.width - 48 : 470)
        height: 190
        modal: true
        title: qsTr("Move event to another account?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        closePolicy: Popup.CloseOnEscape

        contentItem: Text {
            width: crossAccountMoveDialog.availableWidth
            text: qsTr("OmaCalendar will create the destination event first. It will delete the original only after the destination provider acknowledges it.")
            color: Theme.text
            wrapMode: Text.Wrap
            font.pixelSize: Theme.smallFontSize
        }

        onAccepted: {
            const confirmedOptions = Object.assign({}, window.pendingMoveOptions,
                                                   {"confirmedCrossProvider": true})
            const value = window.pendingMoveEvent
            window.pendingMoveEvent = ({})
            window.pendingMoveOptions = ({})
            window.saveEvent(value, confirmedOptions)
        }
        onRejected: {
            window.pendingMoveEvent = ({})
            window.pendingMoveOptions = ({})
        }
    }

    Dialog {
        id: conflictMergeDialog
        property var conflictData: ({})
        property var sourceSnapshot: ({})
        property string validationError: ""

        anchors.centerIn: Overlay.overlay
        width: Math.min(680, Overlay.overlay ? Overlay.overlay.width - 48 : 680)
        height: Math.min(760, Overlay.overlay ? Overlay.overlay.height - 48 : 760)
        modal: true
        title: qsTr("Merge conflicting event")
        standardButtons: Dialog.Cancel
        closePolicy: Popup.CloseOnEscape

        function openFor(value) {
            conflictData = value || ({})
            const local = conflictData.localSnapshot || ({})
            sourceVersionBox.currentIndex = Object.keys(local).length > 0 ? 0 : 1
            loadSnapshot()
            open()
            mergeTitleField.forceActiveFocus()
        }

        function selectedSnapshot() {
            if (sourceVersionBox.currentIndex === 0)
                return conflictData.localSnapshot || ({})
            return conflictData.remoteSnapshot || ({})
        }

        function wallText(value, endValue) {
            if (value.allDay)
                return String(endValue ? value.endDate : value.startDate)
            const utcText = String(endValue ? value.endUtc : value.startUtc)
            if (value.timeKind === "floating")
                return utcText.slice(0, 23)
            return App.utcToWallTime(utcText, value.startTimeZone || "")
        }

        function systemTimeZone() {
            try {
                return Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC"
            } catch (error) {
                return "UTC"
            }
        }

        function loadSnapshot() {
            sourceSnapshot = Object.assign({}, selectedSnapshot())
            validationError = ""
            mergeTitleField.text = sourceSnapshot.summary || ""
            mergeLocationField.text = sourceSnapshot.location || ""
            mergeUrlField.text = sourceSnapshot.url || ""
            mergeNotesField.text = sourceSnapshot.description || ""
            mergeAttendeesField.text = attendeeText(sourceSnapshot.attendees || [])
            mergeAllDay.checked = sourceSnapshot.allDay === true
            mergeTimeKind.currentIndex = sourceSnapshot.timeKind === "floating" ? 1 : 0
            mergeTimeZoneField.text = sourceSnapshot.startTimeZone
                    || systemTimeZone()
            const startWall = wallText(sourceSnapshot, false)
            const endWall = wallText(sourceSnapshot, true)
            mergeStartDate.text = String(startWall).slice(0, 10)
            mergeStartTime.text = String(startWall).slice(11, 16)
            if (sourceSnapshot.allDay) {
                const exclusiveEnd = new Date(String(endWall) + "T00:00:00")
                exclusiveEnd.setDate(exclusiveEnd.getDate() - 1)
                mergeEndDate.text = Qt.formatDate(exclusiveEnd, "yyyy-MM-dd")
            } else {
                mergeEndDate.text = String(endWall).slice(0, 10)
            }
            mergeEndTime.text = String(endWall).slice(11, 16)
            mergeAvailability.currentIndex = sourceSnapshot.transparency
                    === "transparent" ? 1 : 0
            mergeVisibility.currentIndex = visibilityIndex(
                        sourceSnapshot.visibility || "default")
            mergeRecurrenceField.text = sourceSnapshot.recurrenceRule || ""
        }

        function attendeeText(values) {
            const result = []
            for (let index = 0; index < values.length; ++index) {
                const attendee = values[index]
                result.push(typeof attendee === "string"
                            ? attendee : String(attendee.email || ""))
            }
            return result.filter(function(value) { return value.length > 0 }).join(", ")
        }

        function parsedAttendees() {
            const values = mergeAttendeesField.text.split(/[\n,;]/)
            const result = []
            const existing = sourceSnapshot.attendees || []
            for (let index = 0; index < values.length; ++index) {
                const email = values[index].trim()
                if (email.length === 0)
                    continue
                let preserved = null
                for (let candidateIndex = 0; candidateIndex < existing.length;
                     ++candidateIndex) {
                    const candidate = existing[candidateIndex]
                    if (String(candidate.email || "").toLowerCase()
                            === email.toLowerCase()) {
                        preserved = Object.assign({}, candidate)
                        break
                    }
                }
                if (preserved) {
                    preserved.email = email
                    result.push(preserved)
                } else {
                    result.push({"email": email})
                }
            }
            return result
        }

        function visibilityIndex(value) {
            const values = ["default", "public", "private", "confidential"]
            const index = values.indexOf(String(value))
            return index < 0 ? 0 : index
        }

        function floatingIso(dateText, timeText) {
            return dateText + "T" + timeText + ":00.000Z"
        }

        function submitMerge() {
            validationError = ""
            if (!mergeTitleField.text.trim()) {
                validationError = qsTr("Add an event title.")
                return
            }
            const start = new Date(mergeStartDate.text + "T"
                                   + (mergeAllDay.checked ? "00:00" : mergeStartTime.text))
            const end = new Date(mergeEndDate.text + "T"
                                 + (mergeAllDay.checked ? "00:00" : mergeEndTime.text))
            if (isNaN(start.getTime()) || isNaN(end.getTime()) || end < start
                    || (!mergeAllDay.checked && end <= start)) {
                validationError = qsTr("Enter a valid end after the start.")
                return
            }
            if (!mergeAllDay.checked && mergeTimeKind.currentValue !== "floating"
                    && !App.isValidTimeZone(mergeTimeZoneField.text.trim())) {
                validationError = qsTr("Enter a valid IANA time zone.")
                return
            }

            const merged = Object.assign({}, sourceSnapshot)
            merged.summary = mergeTitleField.text.trim()
            merged.location = mergeLocationField.text.trim()
            merged.url = mergeUrlField.text.trim()
            merged.description = mergeNotesField.text
            merged.attendees = parsedAttendees()
            merged.allDay = mergeAllDay.checked
            merged.timeKind = mergeAllDay.checked ? "all_day"
                                                  : mergeTimeKind.currentValue
            merged.transparency = mergeAvailability.currentValue
            merged.visibility = mergeVisibility.currentValue
            merged.recurrenceRule = mergeRecurrenceField.text.trim()
            if (mergeAllDay.checked) {
                const exclusiveEnd = new Date(mergeEndDate.text + "T00:00:00")
                exclusiveEnd.setDate(exclusiveEnd.getDate() + 1)
                merged.startDate = mergeStartDate.text
                merged.endDate = Qt.formatDate(exclusiveEnd, "yyyy-MM-dd")
                merged.startUtc = ""
                merged.endUtc = ""
                merged.startTimeZone = ""
                merged.endTimeZone = ""
            } else {
                merged.startTimeZone = mergeTimeKind.currentValue === "floating"
                        ? "" : mergeTimeZoneField.text.trim()
                merged.endTimeZone = merged.startTimeZone
                merged.startUtc = mergeTimeKind.currentValue === "floating"
                        ? floatingIso(mergeStartDate.text, mergeStartTime.text)
                        : App.wallTimeToUtc(mergeStartDate.text,
                                            mergeStartTime.text,
                                            merged.startTimeZone)
                merged.endUtc = mergeTimeKind.currentValue === "floating"
                        ? floatingIso(mergeEndDate.text, mergeEndTime.text)
                        : App.wallTimeToUtc(mergeEndDate.text,
                                            mergeEndTime.text,
                                            merged.endTimeZone)
                if (!merged.startUtc || !merged.endUtc) {
                    validationError = qsTr("That wall time does not exist in the selected time zone.")
                    return
                }
                merged.startDate = ""
                merged.endDate = ""
            }
            App.resolveConflict(String(conflictData.id), "merge", merged)
            close()
        }

        contentItem: ScrollView {
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: conflictMergeDialog.availableWidth
                spacing: 12

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Start from either saved version, then edit the final event. Provider identity and unsupported fields are preserved.")
                    color: Theme.mutedText
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.smallFontSize
                }
                AppComboBox {
                    id: sourceVersionBox
                    Layout.fillWidth: true
                    model: [qsTr("Start with my version"), qsTr("Start with remote version")]
                    Accessible.name: qsTr("Conflict merge starting version")
                    onActivated: conflictMergeDialog.loadSnapshot()
                }
                AppTextField {
                    id: mergeTitleField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Title")
                    accessibleName: qsTr("Merged event title")
                }
                RowLayout {
                    Layout.fillWidth: true
                    AppCheckBox {
                        id: mergeAllDay
                        text: qsTr("All day")
                        Accessible.name: text
                    }
                    AppComboBox {
                        id: mergeTimeKind
                        Layout.fillWidth: true
                        visible: !mergeAllDay.checked
                        model: [{"text": qsTr("Zoned time"), "value": "zoned"},
                                {"text": qsTr("Floating time"), "value": "floating"}]
                        textRole: "text"
                        valueRole: "value"
                        Accessible.name: qsTr("Merged event time type")
                    }
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 8
                    AppTextField {
                        id: mergeStartDate
                        Layout.fillWidth: true
                        placeholderText: qsTr("Start date · YYYY-MM-DD")
                        accessibleName: qsTr("Merged event start date")
                    }
                    AppTextField {
                        id: mergeStartTime
                        Layout.fillWidth: true
                        visible: !mergeAllDay.checked
                        placeholderText: qsTr("Start time · HH:MM")
                        accessibleName: qsTr("Merged event start time")
                    }
                    AppTextField {
                        id: mergeEndDate
                        Layout.fillWidth: true
                        placeholderText: qsTr("End date · YYYY-MM-DD")
                        accessibleName: qsTr("Merged event end date")
                    }
                    AppTextField {
                        id: mergeEndTime
                        Layout.fillWidth: true
                        visible: !mergeAllDay.checked
                        placeholderText: qsTr("End time · HH:MM")
                        accessibleName: qsTr("Merged event end time")
                    }
                }
                AppTextField {
                    id: mergeTimeZoneField
                    Layout.fillWidth: true
                    visible: !mergeAllDay.checked
                             && mergeTimeKind.currentValue !== "floating"
                    placeholderText: qsTr("IANA time zone · Europe/London")
                    accessibleName: qsTr("Merged event time zone")
                }
                AppTextField {
                    id: mergeLocationField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Location")
                    accessibleName: qsTr("Merged event location")
                }
                AppTextField {
                    id: mergeUrlField
                    Layout.fillWidth: true
                    placeholderText: qsTr("URL or meeting link")
                    accessibleName: qsTr("Merged event URL")
                }
                AppTextField {
                    id: mergeAttendeesField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Guest emails, separated by commas")
                    accessibleName: qsTr("Merged event guests")
                }
                RowLayout {
                    Layout.fillWidth: true
                    AppComboBox {
                        id: mergeAvailability
                        Layout.fillWidth: true
                        model: [{"text": qsTr("Busy"), "value": "opaque"},
                                {"text": qsTr("Free"), "value": "transparent"}]
                        textRole: "text"
                        valueRole: "value"
                        Accessible.name: qsTr("Merged event availability")
                    }
                    AppComboBox {
                        id: mergeVisibility
                        Layout.fillWidth: true
                        model: [{"text": qsTr("Default visibility"), "value": "default"},
                                {"text": qsTr("Public"), "value": "public"},
                                {"text": qsTr("Private"), "value": "private"},
                                {"text": qsTr("Confidential"), "value": "confidential"}]
                        textRole: "text"
                        valueRole: "value"
                        Accessible.name: qsTr("Merged event visibility")
                    }
                }
                AppTextField {
                    id: mergeRecurrenceField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Recurrence rule, for example FREQ=WEEKLY")
                    accessibleName: qsTr("Merged event recurrence rule")
                }
                TextArea {
                    id: mergeNotesField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 100
                    placeholderText: qsTr("Notes")
                    color: Theme.text
                    wrapMode: TextEdit.Wrap
                    Accessible.name: qsTr("Merged event notes")
                }
                Text {
                    visible: conflictMergeDialog.validationError.length > 0
                    Layout.fillWidth: true
                    text: conflictMergeDialog.validationError
                    color: Theme.danger
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.smallFontSize
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    AppButton {
                        text: qsTr("Apply merged event")
                        primary: true
                        onClicked: conflictMergeDialog.submitMerge()
                    }
                }
            }
        }
    }

    ActivityPanel {
        id: activityPanel
        mode: "search"
        searchResults: window.searchResults
        invitations: window.invitations
        conflicts: window.conflicts
        operations: window.operations
        searchResultsModel: App.connected
                            ? window.appValue("searchResultsModel", null) : null
        invitationsModel: window.appValue("invitationsModel", null)
        conflictsModel: window.appValue("conflictsModel", null)
        operationsModel: window.appValue("operationsModel", null)
        calendars: App.calendars
        accounts: App.accounts
        connected: App.connected
        syncing: App.busy
        statusText: App.statusText
        timeFormat: String(window.preferences.timeFormat || "system")
        onSearchRequested: (query, filters) => window.search(query, filters)
        onEventActivated: value => window.openEvent(value)
        onInvitationResponseRequested: (invitationId, recurrenceId,
                                        expectedLocalRevision, response,
                                        recurrenceScope) =>
                                                   window.callApp("respondToInvitation",
                                                                  [invitationId,
                                                                   response,
                                                                   recurrenceScope,
                                                                   recurrenceId,
                                                                   expectedLocalRevision])
        onInvitationSeenRequested: invitationId =>
                                            window.callApp("markInvitationSeen",
                                                           [invitationId])
        onConflictResolutionRequested: (conflictId, strategy, mergedDraft) =>
                                                   window.callApp("resolveConflict",
                                                                  [conflictId,
                                                                   strategy,
                                                                   mergedDraft])
        onConflictMergeRequested: conflictData =>
                                              conflictMergeDialog.openFor(
                                                  conflictData)
        onOperationRetryRequested: operationId =>
                                               window.callApp("retryOperation",
                                                              [operationId])
        onOperationDiscardRequested: operationId =>
                                                 window.callApp("discardOperation",
                                                                [operationId])
        onSyncRequested: App.syncAll()
    }

    AccountSettingsDrawer {
        id: settingsDrawer
        accounts: App.accounts
        calendars: App.calendars
        calendarSets: window.calendarSets
        accountsModel: window.appValue("accountsModel", null)
        calendarsModel: window.appValue("calendarsModel", null)
        calendarSetsModel: window.appValue("calendarSetsModel", null)
        connected: App.connected
        busy: App.busy
        preferences: window.preferences
        systemTimeZoneId: String(window.appValue("systemTimeZoneId", "UTC"))
        availableTimeZoneIds: window.appValue("availableTimeZoneIds", ["UTC"])
        bundledGoogleOAuthAvailable: Boolean(
                                           window.appValue(
                                               "bundledGoogleOAuthAvailable",
                                               false))
        googleOAuthConfigured: Boolean(
                                   window.appValue("googleOAuthConfigured", false))
        onConnectGoogleRequested: displayName => {
            if (settingsDrawer.bundledGoogleOAuthAvailable)
                App.connectGoogle(displayName)
            else
                App.connectGoogleConfigured(displayName)
        }
        onConnectGoogleClientRequested: (clientId, displayName) =>
                                            App.connectGoogleWithClientId(
                                                clientId, displayName)
        onConnectGoogleCredentialsRequested: displayName => {
            window.pendingGoogleDisplayName = displayName
            googleCredentialsFileDialog.open()
        }
        onAddCalDavRequested: (endpoint, username, password, displayName) =>
                                      App.addCalDavAccount(endpoint, username,
                                                          password, displayName)
        onAddLocalCalendarRequested: (name, color) =>
                                             window.callApp("addLocalCalendar",
                                                            [name, color])
        onRemoveCalendarRequested: calendarId => App.removeCalendar(calendarId)
        onAddIcsSubscriptionRequested: (url, username, password, displayName) =>
                                               window.callApp("addIcsSubscription",
                                                              [{"url": url,
                                                                "username": username,
                                                                "password": password,
                                                                "displayName": displayName}])
        onRemoveAccountRequested: (accountId, removeCachedData) => {
            if (!window.callApp("removeAccountWithOptions",
                                [accountId, removeCachedData]))
                App.removeAccount(accountId)
        }
        onReauthorizeAccountRequested: accountId => App.reauthorizeAccount(accountId)
        onUpdateAccountCredentialsRequested: (accountId, username, password) =>
                                                 App.updateAccountCredentials(
                                                     accountId, username, password)
        onSyncAccountRequested: accountId => App.syncAccount(accountId)
        onCalendarPreferenceChanged: (calendarId, key, value) =>
                                                 window.callApp("setCalendarPreference",
                                                                [calendarId, key, value])
        onPreferenceChanged: (key, value) =>
                                     window.callApp("setPreference", [key, value])
        onDiagnosticsRequested: window.callApp("previewDiagnostics", [])
        onImportIcsRequested: importIcsFileDialog.open()
        onExportIcsRequested: icsExportDialog.openForSelection()
        onUpsertCalendarSetRequested: value => App.upsertCalendarSet(value)
        onRemoveCalendarSetRequested: setId => App.removeCalendarSet(setId)
    }

    FileDialog {
        id: googleCredentialsFileDialog
        title: qsTr("Choose Google desktop OAuth credentials")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("JSON files (*.json)"), qsTr("All files (*)")]
        onAccepted: App.connectGoogleWithCredentials(
                        selectedFile, window.pendingGoogleDisplayName)
    }

    FileDialog {
        id: importIcsFileDialog
        title: qsTr("Choose an iCalendar file")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("iCalendar files (*.ics)"), qsTr("All files (*)")]
        onAccepted: window.openIcsImport(selectedFile)
    }

    Dialog {
        id: icsImportDialog
        property url fileUrl
        property var preview: ({})
        anchors.centerIn: Overlay.overlay
        width: Math.min(580, Overlay.overlay ? Overlay.overlay.width - 48 : 580)
        modal: true
        title: qsTr("Import iCalendar events")
        standardButtons: Dialog.Cancel

        contentItem: ColumnLayout {
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: window.localFileName(icsImportDialog.fileUrl)
                color: Theme.text
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle
            }
            AppComboBox {
                id: importCalendarBox
                Layout.fillWidth: true
                model: window.writableCalendars
                textRole: "name"
                valueRole: "id"
                Accessible.name: qsTr("Import destination calendar")
            }
            AppComboBox {
                id: duplicatePolicyBox
                Layout.fillWidth: true
                model: [
                    {"text": qsTr("Skip matching UIDs"), "value": "skip"},
                    {"text": qsTr("Import duplicates as copies"), "value": "copy"},
                    {"text": qsTr("Replace matching UIDs"), "value": "replace"}
                ]
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("Duplicate import handling")
            }
            Text {
                Layout.fillWidth: true
                text: icsImportDialog.preview.count === undefined
                      ? qsTr("Preview the file before importing.")
                      : Number(icsImportDialog.preview.count) + qsTr(" event(s), ")
                        + Number(icsImportDialog.preview.duplicateCount || 0)
                        + qsTr(" matching UID(s)")
                color: Theme.mutedText
                font.pixelSize: Theme.smallFontSize
            }
            ColumnLayout {
                Layout.fillWidth: true
                Repeater {
                    model: (icsImportDialog.preview.events || []).slice(0, 6)
                    delegate: Text {
                        required property var modelData
                        Layout.fillWidth: true
                        text: "• " + (modelData.event.summary || qsTr("Untitled event"))
                              + (modelData.duplicate ? qsTr("  ·  duplicate") : "")
                        color: modelData.duplicate ? Theme.warning : Theme.text
                        font.pixelSize: Theme.smallFontSize
                        elide: Text.ElideRight
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Preview")
                    enabled: importCalendarBox.currentIndex >= 0
                    onClicked: App.previewIcsImport(icsImportDialog.fileUrl,
                                                    importCalendarBox.currentValue)
                }
                AppButton {
                    text: qsTr("Import")
                    primary: true
                    enabled: Number(icsImportDialog.preview.count || 0) > 0
                    onClicked: App.commitIcsImport(icsImportDialog.fileUrl,
                                                   importCalendarBox.currentValue,
                                                   duplicatePolicyBox.currentValue)
                }
            }
        }
    }

    Dialog {
        id: icsExportDialog
        anchors.centerIn: Overlay.overlay
        width: Math.min(520, Overlay.overlay ? Overlay.overlay.width - 48 : 520)
        modal: true
        title: qsTr("Export iCalendar events")
        standardButtons: Dialog.Cancel

        function openForSelection() {
            exportScopeBox.currentIndex = 0
            rangeStartField.text = Qt.formatDate(App.selectedDate, "yyyy-MM-dd")
            const end = new Date(App.selectedDate.getFullYear(),
                                 App.selectedDate.getMonth(),
                                 App.selectedDate.getDate() + 1)
            rangeEndField.text = Qt.formatDate(end, "yyyy-MM-dd")
            open()
        }

        contentItem: ColumnLayout {
            spacing: 12
            AppComboBox {
                id: exportScopeBox
                Layout.fillWidth: true
                model: [qsTr("Date range"), qsTr("Active calendar set"), qsTr("Entire local calendar")]
                Accessible.name: qsTr("Export scope")
            }
            AppComboBox {
                id: exportCalendarSetBox
                visible: exportScopeBox.currentIndex === 1
                Layout.fillWidth: true
                model: window.calendarSets
                textRole: "name"
                valueRole: "id"
                currentIndex: Math.max(0, window.calendarSetIndex(
                                             window.activeCalendarSetId))
                Accessible.name: qsTr("Calendar set to export")
            }
            AppComboBox {
                id: exportLocalCalendarBox
                visible: exportScopeBox.currentIndex === 2
                Layout.fillWidth: true
                model: window.localWritableCalendars
                textRole: "name"
                valueRole: "id"
                Accessible.name: qsTr("Local calendar to export")
            }
            RowLayout {
                visible: exportScopeBox.currentIndex === 0
                Layout.fillWidth: true
                AppTextField {
                    id: rangeStartField
                    Layout.fillWidth: true
                    placeholderText: qsTr("YYYY-MM-DD")
                    accessibleName: qsTr("Export range start")
                }
                Text { text: qsTr("to"); color: Theme.mutedText }
                AppTextField {
                    id: rangeEndField
                    Layout.fillWidth: true
                    placeholderText: qsTr("YYYY-MM-DD")
                    accessibleName: qsTr("Export range end")
                }
            }
            Text {
                visible: exportScopeBox.currentIndex === 2
                         && window.localWritableCalendars.length === 0
                Layout.fillWidth: true
                text: qsTr("Create a local calendar before exporting a whole calendar.")
                color: Theme.warning
                wrapMode: Text.Wrap
                font.pixelSize: Theme.smallFontSize
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Choose destination…")
                    primary: true
                    enabled: exportScopeBox.currentIndex !== 2
                             || exportLocalCalendarBox.currentIndex >= 0
                    onClicked: {
                        const scope = window.exportScope(
                                        exportScopeBox.currentIndex,
                                        exportCalendarSetBox.currentValue,
                                        exportLocalCalendarBox.currentValue,
                                        rangeStartField.text,
                                        rangeEndField.text)
                        if (Object.keys(scope).length > 0) {
                            window.pendingExportScope = scope
                            icsExportDialog.close()
                            exportIcsFileDialog.open()
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: exportIcsFileDialog
        title: qsTr("Save iCalendar export")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "ics"
        nameFilters: [qsTr("iCalendar files (*.ics)")]
        onAccepted: App.exportIcs(window.pendingExportScope, selectedFile)
    }

    Connections {
        target: App
        function onIcsImportPreviewReady(preview) {
            icsImportDialog.preview = preview
        }
        function onIcsImportCompleted(result) {
            icsImportDialog.close()
        }
        function onOpenIcsImportRequested(file) {
            window.openIcsImport(file)
        }
    }

    Shortcut {
        sequence: "Ctrl+N"
        context: Qt.ApplicationShortcut
        onActivated: editor.openNew(App.selectedDate, 540)
    }
    Shortcut {
        sequence: "Ctrl+F"
        context: Qt.ApplicationShortcut
        onActivated: window.openActivity("search")
    }
    Shortcut {
        sequence: "/"
        enabled: !editor.opened && !settingsDrawer.opened
        context: Qt.ApplicationShortcut
        onActivated: window.openActivity("search")
    }
    Shortcut { sequence: "Alt+1"; context: Qt.ApplicationShortcut; onActivated: window.setView("agenda") }
    Shortcut { sequence: "Alt+2"; context: Qt.ApplicationShortcut; onActivated: window.setView("day") }
    Shortcut { sequence: "Alt+3"; context: Qt.ApplicationShortcut; onActivated: window.setView("week") }
    Shortcut { sequence: "Alt+4"; context: Qt.ApplicationShortcut; onActivated: window.setView("month") }
    Shortcut { sequence: "Alt+5"; context: Qt.ApplicationShortcut; onActivated: window.setView("year") }
    Shortcut {
        sequence: "T"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.goToday()
    }
    Shortcut {
        sequence: "["
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.navigatePeriod(-1)
    }
    Shortcut {
        sequence: "]"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.navigatePeriod(1)
    }
    Shortcut {
        sequence: "Left"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.moveSelectionDate(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.moveSelectionDate(1)
    }
    Shortcut {
        sequence: "Up"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.selectAdjacentEvent(-1)
    }
    Shortcut {
        sequence: "Down"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.selectAdjacentEvent(1)
    }
    Shortcut {
        sequence: "Return"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.openKeyboardSelection()
    }
    Shortcut {
        sequence: "Enter"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.openKeyboardSelection()
    }
    Shortcut {
        sequence: "Delete"
        enabled: window.navigationShortcutsEnabled()
                 && Boolean(window.selectedEvent.id)
        context: Qt.ApplicationShortcut
        onActivated: window.requestKeyboardDelete()
    }
    Shortcut {
        sequence: "Ctrl+Z"
        context: Qt.ApplicationShortcut
        onActivated: window.callApp("undoLastMutation", [])
    }

    Connections {
        target: App
        function onEventsChanged() {
            window.firstLoadComplete = true
            window.reconcileSelectedEvent()
            if (activityPanel.searchText.length > 0)
                window.search(activityPanel.searchText,
                              window.activeSearchFilters)
        }
        function onSelectedDateChanged() {
            window.visibleMonth = new Date(App.selectedDate.getFullYear(),
                                           App.selectedDate.getMonth(), 1)
        }
        function onPreferencesChanged() {
            window.applyInitialPreferences()
        }
        function onOpenEventRequested(eventData) {
            window.openEvent(eventData)
        }
        function onCreateEventRequested(draft) {
            editor.openExisting(draft)
        }
        function onOpenSectionRequested(section) {
            if (section === "invitations")
                window.openActivity(section)
            else
                settingsDrawer.open()
        }
        function onWindowActivationRequested() {
            window.showNormal()
            window.raise()
            window.requestActivate()
        }
    }

    Component.onCompleted: {
        applyInitialPreferences()
        loadRangeFor(App.selectedDate, currentView)
    }

    function appValue(name, fallbackValue) {
        try {
            const value = App[name]
            return value === undefined || value === null ? fallbackValue : value
        } catch (error) {
            return fallbackValue
        }
    }

    function viewIndex(viewName) {
        if (viewName === "agenda")
            return 0
        if (viewName === "day")
            return 1
        if (viewName === "week")
            return 2
        if (viewName === "month")
            return 3
        if (viewName === "year")
            return 4
        return 3
    }

    function appList(name) {
        const value = appValue(name, [])
        return value && typeof value.length === "number" ? value : []
    }

    function appObject(name) {
        const value = appValue(name, {})
        return value && typeof value === "object" ? value : {}
    }

    function callApp(methodName, argumentsList) {
        try {
            const method = App[methodName]
            if (typeof method === "function") {
                method.apply(App, argumentsList || [])
                return true
            }
        } catch (error) {
            console.warn("App method unavailable:", methodName, error)
        }
        return false
    }

    function calendarFor(calendarId) {
        for (let index = 0; index < App.calendars.length; ++index) {
            if (App.calendars[index].id === calendarId)
                return App.calendars[index]
        }
        return {}
    }

    function decorateEvents(values) {
        const result = []
        for (let index = 0; index < values.length; ++index) {
            const value = Object.assign({}, values[index])
            const calendar = calendarFor(value.calendarId)
            value.calendarName = calendar.name || qsTr("Calendar")
            value.calendarColor = calendar.colorOverride || calendar.color || Theme.accent
            value.readOnly = value.readOnly === true || calendar.readOnly === true
            result.push(value)
        }
        return result
    }

    function filterVisibleEvents(values) {
        const allowed = activeSetCalendarIds()
        const result = []
        for (let index = 0; index < values.length; ++index) {
            const value = values[index]
            if (!calendarIsVisible(value.calendarId))
                continue
            if (allowed.length > 0 && allowed.indexOf(value.calendarId) < 0)
                continue
            result.push(value)
        }
        return result
    }

    function activeSetCalendarIds() {
        if (!activeCalendarSetId)
            return []
        for (let index = 0; index < calendarSets.length; ++index) {
            const value = calendarSets[index]
            if (String(value.id) === activeCalendarSetId)
                return value.calendarIds || []
        }
        return []
    }

    function calendarIsVisible(calendarId) {
        return hiddenCalendars[calendarId] !== true
    }

    function setCalendarVisible(calendarId, visibleValue) {
        const next = Object.assign({}, hiddenCalendars)
        if (visibleValue)
            delete next[calendarId]
        else
            next[calendarId] = true
        hiddenCalendars = next
        callApp("setCalendarVisibility", [calendarId, visibleValue])
    }

    function activateCalendarSet(setId) {
        callApp("activateCalendarSet", [String(setId || "")])
    }

    function applyInitialPreferences() {
        const savedView = String(preferences.currentView || "month")
        if (["agenda", "day", "week", "month", "year"].indexOf(savedView) >= 0
                && currentView !== savedView) {
            currentView = savedView
            loadRangeFor(App.selectedDate, savedView)
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

    function eventsForDate(dateValue, values) {
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                               dateValue.getDate())
        const end = new Date(start.getFullYear(), start.getMonth(), start.getDate() + 1)
        const result = []
        for (let index = 0; index < values.length; ++index) {
            if (eventStart(values[index]) < end && eventEnd(values[index]) > start)
                result.push(values[index])
        }
        result.sort(function(first, second) {
            if (first.allDay !== second.allDay)
                return first.allDay ? -1 : 1
            return eventStart(first) - eventStart(second)
        })
        return result
    }

    function eventReference(value) {
        if (!value || !value.id)
            return ""
        return String(value.id) + "\n" + String(value.recurrenceId || "")
    }

    function selectedDateEvents() {
        return eventsForDate(App.selectedDate, visibleEvents)
    }

    function reconcileSelectedEvent() {
        const reference = selectedEventReference
        if (!reference)
            return
        for (let index = 0; index < visibleEvents.length; ++index) {
            if (eventReference(visibleEvents[index]) === reference) {
                selectedEvent = visibleEvents[index]
                return
            }
        }
        selectedEvent = ({})
    }

    function selectAdjacentEvent(direction) {
        if (currentView === "year") {
            moveSelectionDate(direction * 7)
            return
        }
        const candidates = selectedDateEvents()
        if (candidates.length === 0) {
            selectedEvent = ({})
            return
        }
        const reference = selectedEventReference
        let selectedIndex = -1
        for (let index = 0; index < candidates.length; ++index) {
            if (eventReference(candidates[index]) === reference) {
                selectedIndex = index
                break
            }
        }
        if (selectedIndex < 0)
            selectedIndex = direction > 0 ? 0 : candidates.length - 1
        else
            selectedIndex = Math.max(0, Math.min(candidates.length - 1,
                                                 selectedIndex + direction))
        selectedEvent = candidates[selectedIndex]
    }

    function moveSelectionDate(days) {
        const next = new Date(App.selectedDate.getFullYear(),
                              App.selectedDate.getMonth(),
                              App.selectedDate.getDate() + days)
        selectedEvent = ({})
        selectDate(next)
    }

    function openKeyboardSelection() {
        if (selectedEventReference) {
            openEvent(selectedEvent)
            return
        }
        const candidates = selectedDateEvents()
        if (candidates.length > 0) {
            selectedEvent = candidates[0]
            openEvent(selectedEvent)
        }
    }

    function selectDate(dateValue) {
        App.setSelectedDate(dateValue)
        visibleMonth = new Date(dateValue.getFullYear(), dateValue.getMonth(), 1)
        loadRangeFor(dateValue, currentView)
    }

    function setView(viewName) {
        currentView = viewName
        let anchorDate = App.selectedDate
        if (viewName === "agenda") {
            anchorDate = new Date()
            App.setSelectedDate(anchorDate)
            visibleMonth = new Date(anchorDate.getFullYear(),
                                    anchorDate.getMonth(), 1)
        }
        callApp("setCurrentView", [viewName])
        loadRangeFor(anchorDate, viewName)
    }

    function loadRangeFor(anchorDate, viewName) {
        let start
        let end
        if (viewName === "day") {
            start = new Date(anchorDate.getFullYear(), anchorDate.getMonth(),
                             anchorDate.getDate() - 1)
            end = new Date(anchorDate.getFullYear(), anchorDate.getMonth(),
                           anchorDate.getDate() + 2)
        } else if (viewName === "week") {
            start = startOfWeek(anchorDate)
            start.setDate(start.getDate() - 1)
            end = new Date(start.getFullYear(), start.getMonth(), start.getDate() + 10)
        } else if (viewName === "agenda") {
            start = new Date(anchorDate.getFullYear(), anchorDate.getMonth(),
                             anchorDate.getDate() - 7)
            end = new Date(anchorDate.getFullYear(), anchorDate.getMonth(),
                           anchorDate.getDate() + 45)
        } else if (viewName === "year") {
            start = new Date(anchorDate.getFullYear(), 0, 1)
            end = new Date(anchorDate.getFullYear(), 11, 31)
        } else {
            start = new Date(anchorDate.getFullYear(), anchorDate.getMonth(), -7)
            end = new Date(anchorDate.getFullYear(), anchorDate.getMonth() + 1, 14)
        }
        App.loadRange(start, end)
    }

    function startOfWeek(dateValue) {
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                               dateValue.getDate())
        const jsFirstDay = firstDayOfWeek === 7 ? 0 : firstDayOfWeek
        const distance = (start.getDay() - jsFirstDay + 7) % 7
        start.setDate(start.getDate() - distance)
        return start
    }

    function navigatePeriod(direction) {
        const dateValue = new Date(App.selectedDate.getFullYear(),
                                   App.selectedDate.getMonth(),
                                   App.selectedDate.getDate())
        if (currentView === "day")
            dateValue.setDate(dateValue.getDate() + direction)
        else if (currentView === "week" || currentView === "agenda")
            dateValue.setDate(dateValue.getDate() + 7 * direction)
        else if (currentView === "year")
            dateValue.setFullYear(dateValue.getFullYear() + direction)
        else
            dateValue.setMonth(dateValue.getMonth() + direction)
        selectDate(dateValue)
    }

    function goToday() {
        selectDate(new Date())
    }

    function periodTitle() {
        if (currentView === "day")
            return Qt.formatDate(App.selectedDate, "dddd, MMMM d")
        if (currentView === "week") {
            const start = startOfWeek(App.selectedDate)
            const end = new Date(start.getFullYear(), start.getMonth(), start.getDate() + 6)
            return start.getMonth() === end.getMonth()
                    ? Qt.formatDate(start, "MMMM d") + "–" + end.getDate()
                    : Qt.formatDate(start, "MMM d") + " – " + Qt.formatDate(end, "MMM d")
        }
        if (currentView === "year")
            return String(App.selectedDate.getFullYear())
        return Qt.formatDate(App.selectedDate, "MMMM yyyy")
    }

    function periodSubtitle() {
        if (currentView === "agenda")
            return qsTr("Upcoming agenda")
        if (currentView === "month")
            return Qt.formatDate(App.selectedDate, "dddd, MMMM d")
        if (currentView === "year")
            return qsTr("Year overview")
        if (currentView === "week")
            return qsTr("Week view")
        return qsTr("Day view")
    }

    function openEvent(value) {
        selectedEvent = value
        editor.openExisting(value)
    }

    function saveEvent(value, options) {
        const mutationOptions = Object.assign({}, options || {})
        const sourceCalendarId = String(mutationOptions.sourceCalendarId || "")
        const targetCalendarId = String(value.calendarId || "")
        if (value.id && sourceCalendarId && targetCalendarId
                && sourceCalendarId !== targetCalendarId
                && calendarAccountId(sourceCalendarId)
                   !== calendarAccountId(targetCalendarId)
                && mutationOptions.confirmedCrossProvider !== true) {
            pendingMoveEvent = value
            pendingMoveOptions = mutationOptions
            crossAccountMoveDialog.open()
            return
        }
        if (callApp("saveEvent", [value, mutationOptions]))
            return
        if (value.id)
            App.updateEvent(value)
        else
            App.createEvent(value)
    }

    function calendarAccountId(calendarId) {
        const values = appList("calendars")
        for (let index = 0; index < values.length; ++index) {
            if (String(values[index].id) === String(calendarId))
                return String(values[index].accountId || "")
        }
        return ""
    }

    function accountProvider(accountId) {
        const values = appList("accounts")
        for (let index = 0; index < values.length; ++index) {
            if (String(values[index].id) === String(accountId))
                return String(values[index].provider || "")
        }
        return ""
    }

    function calendarSetIndex(setId) {
        for (let index = 0; index < calendarSets.length; ++index) {
            if (String(calendarSets[index].id) === String(setId))
                return index
        }
        return 0
    }

    function localFileName(fileUrl) {
        const value = String(fileUrl || "")
        const slash = value.lastIndexOf("/")
        return slash >= 0 ? value.slice(slash + 1) : value
    }

    function openIcsImport(fileUrl) {
        icsImportDialog.fileUrl = fileUrl
        icsImportDialog.preview = ({})
        icsImportDialog.open()
    }

    function exportScope(scopeIndex, calendarSetId, calendarId,
                         startText, endText) {
        if (scopeIndex === 1)
            return {"calendarSetId": String(calendarSetId
                                             || activeCalendarSetId)}
        if (scopeIndex === 2)
            return calendarId ? {"calendarId": String(calendarId)} : ({})
        const start = new Date(String(startText) + "T00:00:00")
        const inclusiveEnd = new Date(String(endText) + "T00:00:00")
        if (isNaN(start.getTime()) || isNaN(inclusiveEnd.getTime())
                || inclusiveEnd < start)
            return ({})
        inclusiveEnd.setDate(inclusiveEnd.getDate() + 1)
        return {"start": start.toISOString(), "end": inclusiveEnd.toISOString()}
    }

    function beginEventExport(eventId) {
        pendingExportScope = {"eventId": String(eventId)}
        editor.close()
        exportIcsFileDialog.open()
    }

    function removeEvent(eventId, options) {
        if (!callApp("requestDeleteEvent", [eventId, options]))
            App.removeEvent(eventId)
        selectedEvent = ({})
    }

    function duplicateEvent(value) {
        const copy = Object.assign({}, value)
        delete copy.id
        delete copy.remoteId
        delete copy.uid
        delete copy.etag
        delete copy.recurrenceId
        copy.summary = (copy.summary || qsTr("Untitled event")) + qsTr(" copy")
        copy.dirty = false
        copy.conflict = false
        editor.openExisting(copy)
    }

    function rescheduleEvent(value, dateValue, startMinute, durationMinutes) {
        if (!eventEditable(value))
            return
        const revision = Number(value.localRevision)
        if (!isFinite(revision) || revision < 0) {
            console.warn("Cannot reschedule event without a local revision")
            return
        }
        const updated = Object.assign({}, value)
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                               dateValue.getDate(), Math.floor(startMinute / 60),
                               startMinute % 60)
        updated.allDay = false
        updated.startUtc = utcForDisplayedWall(start, value.timeKind)
        if (!updated.startUtc)
            return
        updated.endUtc = new Date(new Date(updated.startUtc).getTime()
                                  + durationMinutes * 60000).toISOString()
        updated.startDate = ""
        updated.endDate = ""
        updated.localRevision = revision
        submitInteractionMutation(updated, value)
    }

    function moveEventToDate(value, dateValue) {
        if (!eventEditable(value))
            return
        const revision = Number(value.localRevision)
        if (!isFinite(revision) || revision < 0) {
            console.warn("Cannot move event without a local revision")
            return
        }
        const start = eventStart(value)
        const end = eventEnd(value)
        if (sameDate(start, dateValue))
            return
        const duration = end - start
        const updated = Object.assign({}, value)
        if (value.allDay) {
            updated.startDate = Qt.formatDate(dateValue, "yyyy-MM-dd")
            const days = Math.max(1, Math.round(duration / 86400000))
            const newEnd = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                    dateValue.getDate() + days)
            updated.endDate = Qt.formatDate(newEnd, "yyyy-MM-dd")
        } else {
            const newStart = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                      dateValue.getDate(), start.getHours(),
                                      start.getMinutes())
            updated.startUtc = utcForDisplayedWall(newStart, value.timeKind)
            if (!updated.startUtc)
                return
            updated.endUtc = new Date(new Date(updated.startUtc).getTime()
                                      + duration).toISOString()
        }
        updated.localRevision = revision
        submitInteractionMutation(updated, value)
    }

    function submitInteractionMutation(updated, source) {
        const options = {
            "expectedLocalRevision": Number(source.localRevision),
            "recurrenceScope": source.recurrenceId ? "occurrence" : "series",
            "guestNotificationPolicy": "none",
            "sourceCalendarId": String(source.calendarId || ""),
            "recurrenceId": String(source.recurrenceId || "")
        }
        if (mutationConfirmation.needsChoiceFor(source)) {
            mutationConfirmation.openFor(
                        source, qsTr("Apply change"),
                        {"kind": "save", "draft": updated}, options,
                        futureScopeSupportedForEvent(source))
            return
        }
        performInteractionMutation(updated, options)
    }

    function performInteractionMutation(updated, options) {
        if (!callApp("saveEvent", [updated, options]))
            console.warn("AppController.saveEvent is unavailable")
    }

    function requestKeyboardDelete() {
        const value = selectedEvent
        if (!value || !value.id || !eventEditable(value))
            return
        const options = {
            "expectedLocalRevision": Number(value.localRevision),
            "recurrenceScope": value.recurrenceId ? "occurrence" : "series",
            "guestNotificationPolicy": "none",
            "recurrenceId": String(value.recurrenceId || "")
        }
        if (mutationConfirmation.needsChoiceFor(value)) {
            mutationConfirmation.openFor(
                        value, qsTr("Delete event"),
                        {"kind": "delete", "eventId": String(value.id)}, options,
                        futureScopeSupportedForEvent(value))
            return
        }
        removeEvent(String(value.id), options)
    }

    function futureScopeSupportedForEvent(value) {
        const calendar = calendarFor(value.calendarId)
        const capabilities = calendar.capabilities || ({})
        return capabilities.thisAndFuture === true
    }

    function eventEditable(value) {
        if (!value || value.readOnly === true || value.conflict === true
                || value.dirty === true)
            return false
        const calendar = calendarFor(value.calendarId)
        if (calendar.readOnly === true)
            return false
        const operationState = String(value.operationState || value.syncState || "")
        return operationState !== "pending" && operationState !== "sending"
                && operationState !== "blocked" && operationState !== "retry_wait"
                && operationState !== "failed" && operationState !== "error"
    }

    function utcForDisplayedWall(dateValue, timeKind) {
        if (timeKind === "floating") {
            return new Date(Date.UTC(dateValue.getFullYear(), dateValue.getMonth(),
                                     dateValue.getDate(), dateValue.getHours(),
                                     dateValue.getMinutes(), 0)).toISOString()
        }
        return App.wallTimeToUtc(Qt.formatDate(dateValue, "yyyy-MM-dd"),
                                 Qt.formatTime(dateValue, "HH:mm"),
                                 String(preferences.displayTimeZone || ""))
    }

    function search(query, filters) {
        const normalized = String(query || "").trim().toLowerCase()
        if (normalized.length === 0) {
            localSearchResults = []
            activeSearchFilters = ({})
            return
        }
        const result = []
        const requestedFilters = filters || ({})
        activeSearchFilters = requestedFilters
        const calendarIds = requestedFilters.calendarIds || []
        const rangeStart = requestedFilters.start
                ? new Date(requestedFilters.start) : null
        const rangeEnd = requestedFilters.end
                ? new Date(requestedFilters.end) : null
        for (let index = 0; index < decoratedEvents.length; ++index) {
            const value = decoratedEvents[index]
            if (calendarIds.length > 0
                    && calendarIds.indexOf(String(value.calendarId)) < 0)
                continue
            if (requestedFilters.accountId
                    && calendarAccountId(value.calendarId)
                       !== String(requestedFilters.accountId))
                continue
            const start = eventStart(value)
            const end = eventEnd(value)
            if (rangeStart && end <= rangeStart)
                continue
            if (rangeEnd && start >= rangeEnd)
                continue
            const attendees = value.attendees || []
            if (requestedFilters.invitationState) {
                let matchingResponse = false
                for (let attendeeIndex = 0; attendeeIndex < attendees.length;
                     ++attendeeIndex) {
                    const response = String(attendees[attendeeIndex].responseStatus
                                            || attendees[attendeeIndex].partstat
                                            || "").toLowerCase()
                    if (response === String(requestedFilters.invitationState)
                                      .toLowerCase()) {
                        matchingResponse = true
                        break
                    }
                }
                if (!matchingResponse)
                    continue
            }
            let haystack = String(value.summary || "") + "\n"
                    + String(value.description || "") + "\n"
                    + String(value.location || "")
            for (let attendeeIndex = 0; attendeeIndex < attendees.length; ++attendeeIndex)
                haystack += "\n" + String(attendees[attendeeIndex].email || "")
            if (haystack.toLowerCase().indexOf(normalized) >= 0)
                result.push(value)
        }
        localSearchResults = result
        callApp("searchEvents", [query, filters || {}])
    }

    function openActivity(modeName) {
        activityPanel.mode = modeName
        activityPanel.open()
    }

    function failedOperationCount() {
        let count = 0
        for (let index = 0; index < operations.length; ++index) {
            if (operations[index].state === "blocked"
                    || operations[index].state === "retry_wait")
                ++count
        }
        return count
    }

    function navigationShortcutsEnabled() {
        return !editor.opened && !settingsDrawer.opened && !activityPanel.opened
    }
}
