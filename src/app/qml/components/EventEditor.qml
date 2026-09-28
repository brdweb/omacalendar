pragma ComponentBehavior: Bound
// App is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

Dialog {
    id: editor

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(680, Overlay.overlay ? Overlay.overlay.width - 48 : 680)
    height: Math.min(760, Overlay.overlay ? Overlay.overlay.height - 48 : 760)
    padding: 0
    closePolicy: Popup.CloseOnEscape

    property var eventData: ({})
    property bool editing: Boolean(eventData && eventData.id)
    property bool deleteArmed: false
    property string validationError: ""
    property int defaultStartMinute: 540
    property int defaultDurationMinutes: 60
    property int workDayStart: 8
    property int workDayEnd: 18
    // Start (UTC ISO) of the free/busy window this editor last asked for, so a
    // stale answer for another event is not shown.
    property string freeBusyWindowStart: ""
    property string freeTimeMessage: ""
    readonly property var guestEmails: attendeeEditor.attendees.map(function(value) {
        return String(value.email || "")
    }).filter(function(email) { return email.length > 0 })
    readonly property bool showAvailability: guestEmails.length > 0 && !allDay.checked
                                             && Boolean(App.freeBusy && App.freeBusy.start)
                                             && Date.parse(App.freeBusy.start)
                                                === Date.parse(freeBusyWindowStart)
    property string defaultCalendarId: ""
    readonly property var sourceCalendar: calendarForId(eventData.calendarId || "")
    readonly property bool readOnly: Boolean(eventData && eventData.readOnly)
                                     || sourceCalendar.readOnly === true
    readonly property bool recurring: Boolean(eventData && (eventData.recurrenceRule
                                                              || eventData.recurrenceId))
    readonly property bool hasGuests: attendeeEditor.hasGuests
    readonly property var writableCalendars: App.calendars.filter(
                                                 function(calendar) {
                                                     return calendar.enabled !== false
                                                             && !calendar.readOnly
                                                 })
    readonly property var activeCalendar: calendarForId(
                                              calendarBox.currentIndex >= 0
                                              && calendarBox.currentIndex
                                                 < writableCalendars.length
                                              ? writableCalendars[calendarBox.currentIndex].id
                                              : eventData.calendarId || "")
    readonly property var activeCapabilities: activeCalendar.capabilities || ({})
    readonly property string activeProvider: String(activeCapabilities.provider || "")
    readonly property bool movingCalendars: editing
                                            && String(eventData.calendarId || "")
                                               !== String(activeCalendar.id || "")
    readonly property bool attendeeEditingSupported:
        activeCapabilities.attendeeWrites === true
        || activeCapabilities.attendees === true
        || activeProvider === "google"
    readonly property bool recurrenceEditingSupported:
        activeCapabilities.recurringEvents !== false
        && activeCapabilities.recurrence !== false
    readonly property bool reminderEditingSupported:
        activeCapabilities.reminders !== false
    readonly property bool futureScopeSupported: !movingCalendars
                                                 && activeCapabilities.thisAndFuture
                                                    === true
    // Google calendars list the conference types they accept.
    readonly property bool meetSupported: activeProvider === "google"
                                          && ((activeCapabilities.conferenceProperties || {})
                                              .allowedConferenceSolutionTypes || [])
                                             .indexOf("hangoutsMeet") >= 0
    readonly property string conferenceUrl: String(eventData.conferenceUrl || "")
    readonly property var attachments: {
        const value = App.eventAttachments || ({})
        return editing && value.eventId === eventData.id
                && String(value.recurrenceId || "") === String(eventData.recurrenceId || "")
                ? (value.attachments || []) : []
    }
    readonly property bool futureScopeCheckAvailable: !readOnly && !movingCalendars
                                                       && activeProvider === "caldav"
                                                       && activeCalendar.enabled !== false

    signal saveRequested(var eventData, var mutationOptions)
    signal removeRequested(string eventId, var mutationOptions)
    signal duplicateRequested(var eventData)
    signal exportRequested(string eventId)
    signal joinRequested(string url)

    onFutureScopeSupportedChanged: {
        if (!futureScopeSupported && scopeBox.currentValue === "future")
            scopeBox.currentIndex = 0
    }

    function openNew(dateValue, startMinute, durationMinutes) {
        eventData = ({})
        editing = false
        deleteArmed = false
        validationError = ""
        defaultStartMinute = typeof startMinute === "number" ? startMinute : 540
        titleField.text = ""
        locationField.text = ""
        urlField.text = ""
        notesField.text = ""
        meetBox.checked = false
        attendeeEditor.organizerEmail = ""
        attendeeEditor.load([])
        allDay.checked = false
        timeKindBox.currentIndex = 0
        const startValue = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                    dateValue.getDate(),
                                    Math.floor(defaultStartMinute / 60),
                                    defaultStartMinute % 60)
        const requestedDuration = typeof durationMinutes === "number"
                ? durationMinutes : defaultDurationMinutes
        const endValue = new Date(startValue.getTime()
                                  + Math.max(15, requestedDuration) * 60000)
        startDateField.text = Qt.formatDate(startValue, "yyyy-MM-dd")
        endDateField.text = Qt.formatDate(endValue, "yyyy-MM-dd")
        startTimeField.text = Qt.formatTime(startValue, "HH:mm")
        endTimeField.text = Qt.formatTime(endValue, "HH:mm")
        timeZoneBox.currentIndex = timeZoneIndex(systemTimeZone())
        calendarBox.currentIndex = calendarIndex(defaultCalendarId)
        availabilityBox.currentIndex = 0
        visibilityBox.currentIndex = 0
        recurrenceEditor.load("")
        scopeBox.currentIndex = 0
        notificationBox.currentIndex = 0
        reminderModel.clear()
        reminderModel.append({"kind": "relative", "minutes": 15,
                              "method": "popup", "providerDefault": false,
                              "absoluteAt": "", "sourceIndex": -1,
                              "edited": true})
        open()
        titleField.forceActiveFocus()
    }

    // Opens a new event pre-filled from a quick-add draft; fallbackDate is used
    // when the text named no day.
    function openDraft(draft, fallbackDate) {
        const date = draft.date ? new Date(draft.date + "T00:00:00") : fallbackDate
        openNew(date, draft.startMinute >= 0 ? draft.startMinute : defaultStartMinute,
                draft.durationMinutes > 0 ? draft.durationMinutes : undefined)
        titleField.text = draft.title || ""
        locationField.text = draft.location || ""
        if (draft.allDay) {
            allDay.checked = true
            if (draft.endDate) {
                const inclusiveEnd = new Date(draft.endDate + "T00:00:00")
                inclusiveEnd.setDate(inclusiveEnd.getDate() - 1)
                endDateField.text = Qt.formatDate(inclusiveEnd, "yyyy-MM-dd")
            }
        }
        recurrenceEditor.load(draft.recurrenceRule || "")
    }

    // Asks the daemon who is busy over the week starting on the event's day.
    function requestFreeBusy() {
        freeTimeMessage = ""
        if (!opened || allDay.checked || guestEmails.length === 0
                || typeof App.queryFreeBusy !== "function")
            return
        const start = App.wallTimeToUtc(startDateField.text, "00:00", selectedTimeZone())
        if (!start)
            return
        freeBusyWindowStart = start
        const end = new Date(Date.parse(start) + 7 * 86400000).toISOString()
        App.queryFreeBusy(start, end, guestEmails,
                          writableCalendars[calendarBox.currentIndex]
                          ? String(writableCalendars[calendarBox.currentIndex].id) : "",
                          String(eventData.id || ""), String(eventData.recurrenceId || ""))
    }

    // Moves the event, keeping its length, to the next time everyone shown is
    // free within working hours.
    function findFreeTime() {
        const freeBusy = App.freeBusy || ({})
        // Guests still being checked would otherwise count as free.
        if ((freeBusy.pending || []).length > 0) {
            freeTimeMessage = qsTr("Still checking who is free.")
            return
        }
        let busy = (freeBusy.self || []).slice()
        const attendees = freeBusy.attendees || ({})
        for (const email of guestEmails) {
            const known = attendees[email.toLowerCase()]
            if (known)
                busy = busy.concat(known)
        }
        const zone = selectedTimeZone()
        const startUtc = App.wallTimeToUtc(startDateField.text, startTimeField.text, zone)
        const endUtc = App.wallTimeToUtc(endDateField.text, endTimeField.text, zone)
        let duration = Math.round((Date.parse(endUtc) - Date.parse(startUtc)) / 60000)
        if (!(duration > 0))
            duration = defaultDurationMinutes
        const slot = typeof App.nextFreeSlot === "function"
                ? App.nextFreeSlot(busy, startUtc, duration, workDayStart, workDayEnd,
                                   freeBusy.end, zone)
                : ""
        if (!slot) {
            freeTimeMessage = qsTr("No time in the next week when everyone checked is free.")
            return
        }
        const slotStart = String(App.utcToWallTime(slot, zone))
        const slotEnd = String(App.utcToWallTime(
                                   new Date(Date.parse(slot) + duration * 60000).toISOString(),
                                   zone))
        freeTimeMessage = Date.parse(slot) === Date.parse(startUtc)
                ? qsTr("Everyone checked is free at this time.")
                : qsTr("Moved to the next time everyone checked is free.")
        startDateField.text = slotStart.slice(0, 10)
        startTimeField.text = slotStart.slice(11, 16)
        endDateField.text = slotEnd.slice(0, 10)
        endTimeField.text = slotEnd.slice(11, 16)
    }

    Timer {
        id: freeBusyDelay
        interval: 400
        onTriggered: editor.requestFreeBusy()
    }
    onGuestEmailsChanged: freeBusyDelay.restart()
    onOpened: freeBusyDelay.restart()
    Connections {
        target: startDateField
        function onTextChanged() { freeBusyDelay.restart() }
    }
    Connections {
        target: calendarBox
        function onActivated() { freeBusyDelay.restart() }
    }
    Connections {
        target: timeZoneBox
        function onActivated() { freeBusyDelay.restart() }
    }
    Connections {
        target: allDay
        function onToggled() { freeBusyDelay.restart() }
    }

    function openExisting(value) {
        eventData = value || ({})
        editing = Boolean(eventData.id)
        deleteArmed = false
        validationError = ""
        titleField.text = eventData.summary || ""
        locationField.text = eventData.location || ""
        urlField.text = eventData.url || eventData.meetingUrl || ""
        notesField.text = eventData.description || ""
        meetBox.checked = false
        if (editing && typeof App.loadEventAttachments === "function")
            App.loadEventAttachments(String(eventData.id), String(eventData.recurrenceId || ""))
        attendeeEditor.organizerEmail = String((eventData.organizer || {}).email || "")
        attendeeEditor.load(eventData.attendees || [])
        allDay.checked = eventData.allDay === true
        timeKindBox.currentIndex = eventData.timeKind === "floating" ? 1 : 0
        const start = eventData.allDay
                ? new Date(eventData.startDate + "T00:00:00")
                : new Date(eventData.eventStartLocal
                           || displayTimedDate(eventData.startUtc,
                                               eventData.timeKind))
        const end = eventData.allDay
                ? new Date(eventData.endDate + "T00:00:00")
                : new Date(eventData.eventEndLocal
                           || displayTimedDate(eventData.endUtc,
                                               eventData.timeKind))
        startDateField.text = Qt.formatDate(start, "yyyy-MM-dd")
        endDateField.text = Qt.formatDate(eventData.allDay
                                         ? new Date(end.getFullYear(), end.getMonth(),
                                                    end.getDate() - 1) : end,
                                         "yyyy-MM-dd")
        startTimeField.text = Qt.formatTime(start, "HH:mm")
        endTimeField.text = Qt.formatTime(end, "HH:mm")
        timeZoneBox.currentIndex = timeZoneIndex(eventData.startTimeZone
                                                 || systemTimeZone())
        calendarBox.currentIndex = calendarIndex(eventData.calendarId)
        availabilityBox.currentIndex = eventData.transparency === "transparent" ? 1 : 0
        visibilityBox.currentIndex = visibilityIndex(eventData.visibility || "default")
        recurrenceEditor.load(eventData.recurrenceRule || "")
        scopeBox.currentIndex = 0
        notificationBox.currentIndex = 0
        reminderModel.clear()
        const reminders = eventData.reminders || []
        for (let index = 0; index < reminders.length; ++index) {
            const value = reminders[index]
            const objectValue = value && typeof value === "object"
                    ? value : ({})
            reminderModel.append({
                "kind": reminderKind(value),
                "minutes": reminderMinutes(value),
                "method": String(objectValue.method || "popup"),
                "providerDefault": objectValue.providerDefault === true,
                "absoluteAt": typeof objectValue.at === "string"
                              ? objectValue.at : "",
                "sourceIndex": index,
                "edited": false
            })
        }
        open()
        titleField.forceActiveFocus()
    }

    function dateTime(dateText, timeText) {
        return new Date(dateText + "T" + timeText + ":00")
    }

    function submit() {
        validationError = validateFields()
        if (validationError.length > 0)
            return

        const value = Object.assign({}, eventData || {})
        value.summary = titleField.text.trim()
        value.description = notesField.text
        value.location = locationField.text.trim()
        value.url = urlField.text.trim()
        value.calendarId = writableCalendars[calendarBox.currentIndex].id
        value.allDay = allDay.checked
        value.timeKind = allDay.checked ? "all_day" : timeKindBox.currentValue
        value.startTimeZone = value.timeKind === "floating"
                ? "" : selectedTimeZone()
        value.endTimeZone = value.startTimeZone
        value.transparency = availabilityBox.currentIndex === 1
                ? "transparent" : "opaque"
        value.availability = availabilityBox.currentValue
        value.visibility = visibilityBox.currentValue
        value.attendees = parsedAttendees()
        value.reminders = reminderValues()
        value.recurrenceRule = recurrenceRule()
        if (meetBox.visible && meetBox.checked)
            value.addConference = true
        else
            delete value.addConference

        if (allDay.checked) {
            const inclusiveEnd = new Date(endDateField.text + "T00:00:00")
            inclusiveEnd.setDate(inclusiveEnd.getDate() + 1)
            value.startDate = startDateField.text
            value.endDate = Qt.formatDate(inclusiveEnd, "yyyy-MM-dd")
            value.startUtc = ""
            value.endUtc = ""
        } else {
            value.startUtc = value.timeKind === "floating"
                    ? floatingIso(startDateField.text, startTimeField.text)
                    : App.wallTimeToUtc(startDateField.text, startTimeField.text,
                                        value.startTimeZone)
            value.endUtc = value.timeKind === "floating"
                    ? floatingIso(endDateField.text, endTimeField.text)
                    : App.wallTimeToUtc(endDateField.text, endTimeField.text,
                                        value.endTimeZone)
            if (!value.startUtc || !value.endUtc) {
                validationError = qsTr("That wall time does not exist in the selected time zone.")
                return
            }
            value.startDate = ""
            value.endDate = ""
        }

        editor.saveRequested(value, mutationOptions())
        editor.close()
    }

    function requestDelete() {
        if (!deleteArmed) {
            deleteArmed = true
            deleteReset.restart()
            return
        }
        editor.removeRequested(String(eventData.id), mutationOptions())
        editor.close()
    }

    function validateFields() {
        if (readOnly)
            return qsTr("This event belongs to a read-only calendar.")
        if (!titleField.text.trim())
            return qsTr("Add an event title.")
        if (calendarBox.currentIndex < 0)
            return qsTr("Choose a writable calendar.")
        const startDate = new Date(startDateField.text + "T00:00:00")
        const endDate = new Date(endDateField.text + "T00:00:00")
        if (isNaN(startDate.getTime()) || isNaN(endDate.getTime()))
            return qsTr("Enter valid start and end dates.")
        if (allDay.checked) {
            if (endDate < startDate)
                return qsTr("The end date must be on or after the start date.")
        } else {
            const start = dateTime(startDateField.text, startTimeField.text)
            const end = dateTime(endDateField.text, endTimeField.text)
            if (isNaN(start.getTime()) || isNaN(end.getTime()))
                return qsTr("Enter valid start and end times.")
            if (end <= start)
                return qsTr("The event must end after it starts.")
            if (timeKindBox.currentValue !== "floating"
                    && !App.isValidTimeZone(selectedTimeZone()))
                return qsTr("Enter a valid IANA time zone.")
        }
        // Commit a typed address first: an entry left pending while
        // suggestions showed would otherwise be dropped on save unchecked.
        attendeeEditor.commitInput()
        const attendeeError = attendeeEditor.validationError()
        if (attendeeError)
            return attendeeError
        const recurrenceError = recurrenceEditor.validationError()
        if (recurrenceError)
            return recurrenceError
        if (recurrenceEditor.mode > 0 && !recurrenceEditingSupported)
            return qsTr("This calendar cannot write recurring events.")
        if (hasGuests && !attendeeEditingSupported)
            return qsTr("This calendar cannot write guests or invitations.")
        if (editing && recurring && !scopeBox.currentValue)
            return qsTr("Choose which recurring events this change applies to.")
        if (hasGuests && !notificationBox.currentValue)
            return qsTr("Choose whether guests should be notified.")
        return ""
    }

    function mutationOptions() {
        return {
            "recurrenceScope": recurring ? scopeBox.currentValue : "series",
            "guestNotificationPolicy": hasGuests
                                       ? notificationBox.currentValue : "none",
            "sourceCalendarId": eventData.calendarId || "",
            "recurrenceId": eventData.recurrenceId || ""
        }
    }

    function recurrenceRule() {
        return recurrenceEditor.rule()
    }

    function parsedAttendees() {
        return attendeeEditor.result()
    }

    function reminderValues() {
        const result = []
        for (let index = 0; index < reminderModel.count; ++index) {
            const item = reminderModel.get(index)
            const source = sourceReminder(item.sourceIndex)
            if (!item.edited && item.sourceIndex >= 0) {
                result.push(source)
                continue
            }

            const reminder = source && typeof source === "object"
                    && !Array.isArray(source) ? Object.assign({}, source) : ({})
            reminder.method = item.method || "popup"
            if (item.kind === "absolute") {
                reminder.at = item.absoluteAt
                delete reminder.minutesBefore
                delete reminder.offsetMinutes
                delete reminder.minutes
                delete reminder.min
            } else {
                reminder.minutes = item.minutes
                delete reminder.at
                delete reminder.minutesBefore
                delete reminder.offsetMinutes
                delete reminder.min
            }
            if (item.providerDefault === true) {
                reminder.providerDefault = true
            } else {
                delete reminder.providerDefault
            }
            result.push(reminder)
        }
        return result
    }

    function sourceReminder(index) {
        const reminders = eventData.reminders || []
        return index >= 0 && index < reminders.length ? reminders[index] : null
    }

    function reminderOffset(value) {
        if (typeof value === "number")
            return value
        if (!value || typeof value !== "object")
            return NaN
        const keys = ["minutesBefore", "offsetMinutes", "minutes", "min"]
        for (let index = 0; index < keys.length; ++index) {
            if (value[keys[index]] !== undefined)
                return Number(value[keys[index]])
        }
        return NaN
    }

    function reminderMinutes(value) {
        const offset = reminderOffset(value)
        return isFinite(offset) ? Math.round(offset) : 0
    }

    function reminderKind(value) {
        if (value && typeof value === "object"
                && typeof value.at === "string"
                && value.at.trim().length > 0)
            return "absolute"

        const objectValue = value && typeof value === "object" ? value : ({})
        const method = String(objectValue.method || "popup").toLowerCase()
        const supportedMethod = method === "popup" || method === "display"
                || method === "email" || method === "audio"
        const offset = reminderOffset(value)
        return supportedMethod && isFinite(offset) && offset >= 0
                && Math.floor(offset) === offset ? "relative" : "unsupported"
    }

    function reminderMethodLabel(method) {
        const normalized = String(method || "popup").toLowerCase()
        if (normalized === "email")
            return qsTr("Email")
        if (normalized === "audio")
            return qsTr("Audio")
        return qsTr("Popup")
    }

    function absoluteReminderText(method, value) {
        const parsed = new Date(value)
        const timestamp = isNaN(parsed.getTime())
                ? String(value) : Qt.formatDateTime(parsed, Locale.ShortFormat)
        return reminderMethodLabel(method) + qsTr(" at ") + timestamp
    }

    function calendarIndex(calendarId) {
        for (let index = 0; index < writableCalendars.length; ++index) {
            if (writableCalendars[index].id === calendarId)
                return index
        }
        return writableCalendars.length > 0 ? 0 : -1
    }

    function calendarForId(calendarId) {
        const calendars = App.calendars || []
        for (let index = 0; index < calendars.length; ++index) {
            if (String(calendars[index].id) === String(calendarId))
                return calendars[index]
        }
        return ({})
    }

    function displayTimedDate(isoValue, kind) {
        const parsed = new Date(isoValue)
        if (kind !== "floating")
            return parsed
        return new Date(parsed.getUTCFullYear(), parsed.getUTCMonth(),
                        parsed.getUTCDate(), parsed.getUTCHours(),
                        parsed.getUTCMinutes(), parsed.getUTCSeconds())
    }

    function floatingIso(dateText, timeText) {
        const dateParts = dateText.split("-")
        const timeParts = timeText.split(":")
        return new Date(Date.UTC(Number(dateParts[0]), Number(dateParts[1]) - 1,
                                 Number(dateParts[2]), Number(timeParts[0]),
                                 Number(timeParts[1]), 0)).toISOString()
    }

    function visibilityIndex(value) {
        const values = ["default", "public", "private", "confidential"]
        const index = values.indexOf(value)
        return index >= 0 ? index : 0
    }

    function minuteText(minutes) {
        const hour = Math.floor(minutes / 60)
        const minute = minutes % 60
        return String(hour).padStart(2, "0") + ":"
                + String(minute).padStart(2, "0")
    }

    function systemTimeZone() {
        return String(App.systemTimeZoneId || "UTC")
    }

    function timeZoneIndex(value) {
        const target = String(value || systemTimeZone())
        for (let index = 0; index < App.availableTimeZoneIds.length; ++index) {
            if (String(App.availableTimeZoneIds[index]) === target)
                return index
        }
        return 0
    }

    function selectedTimeZone() {
        return timeZoneBox.currentIndex >= 0
                ? String(App.availableTimeZoneIds[timeZoneBox.currentIndex])
                : systemTimeZone()
    }

    background: Rectangle {
        radius: Theme.radiusLG
        color: Theme.surface
        border.color: Theme.border
    }

    header: Rectangle {
        implicitHeight: 66
        color: "transparent"
        border.color: Theme.divider

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingXL
            anchors.rightMargin: Theme.spacingLG
            spacing: Theme.spacingSM

            AppCloseButton {
                toolTipText: qsTr("Close event editor")
                onClicked: editor.close()
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    textFormat: Text.PlainText
                    text: editor.editing ? qsTr("Event details") : qsTr("New event")
                    color: Theme.text
                    font.pixelSize: Theme.fontSize + 5
                    font.weight: Font.Bold
                }
                Text {
                    textFormat: Text.PlainText
                    visible: editor.eventData.calendarName !== undefined
                    text: editor.eventData.calendarName || ""
                    color: Theme.mutedText
                    font.pixelSize: Theme.microFontSize
                }
            }
            StatusBadge {
                visible: editor.eventData.dirty === true
                         || editor.eventData.conflict === true
                         || editor.readOnly
                text: editor.eventData.conflict === true ? qsTr("Conflict")
                                                         : editor.readOnly ? qsTr("Read only")
                                                                           : qsTr("Pending")
                tone: editor.eventData.conflict === true ? "danger" : "info"
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: editor.availableWidth - 36
                x: 18
                spacing: Theme.spacingMD

                Item { Layout.preferredHeight: 4 }

                Rectangle {
                    visible: editor.readOnly || editor.eventData.conflict === true
                    Layout.fillWidth: true
                    implicitHeight: stateMessage.implicitHeight + 20
                    radius: Theme.radiusMD
                    color: Theme.alpha(editor.eventData.conflict === true
                                       ? Theme.danger : Theme.info, 0.1)
                    border.color: Theme.alpha(editor.eventData.conflict === true
                                              ? Theme.danger : Theme.info, 0.28)
                    Text {
                        textFormat: Text.PlainText
                        id: stateMessage
                        anchors.fill: parent
                        anchors.margins: Theme.spacingSM
                        text: editor.eventData.conflict === true
                              ? qsTr("A newer local or remote update is being selected in the background.")
                              : qsTr("This calendar is read-only. You can inspect details or duplicate the event into a writable calendar.")
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                        wrapMode: Text.Wrap
                    }
                }

                SectionLabel { text: qsTr("EVENT") }
                AppTextField {
                    id: titleField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Event title")
                    accessibleName: qsTr("Event title")
                    enabled: !editor.readOnly
                    font.pixelSize: Theme.fontSize + 2
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingSM
                    AppComboBox {
                        id: calendarBox
                        objectName: "eventCalendar"
                        Layout.fillWidth: true
                        model: editor.writableCalendars
                        textRole: "name"
                        valueRole: "id"
                        enabled: !editor.readOnly
                        Accessible.name: qsTr("Calendar")
                    }
                    AppCheckBox {
                        id: allDay
                        text: qsTr("All day")
                        enabled: !editor.readOnly
                        Accessible.name: qsTr("All-day event")
                    }
                }

                SectionLabel { text: qsTr("DATE & TIME") }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: Theme.spacingSM
                    rowSpacing: Theme.spacingSM

                    Text {
                        textFormat: Text.PlainText
                        text: qsTr("Starts")
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                    }
                    AppTextField {
                        id: startDateField
                        objectName: "eventStartDate"
                        Layout.fillWidth: true
                        placeholderText: qsTr("YYYY-MM-DD")
                        accessibleName: qsTr("Start date")
                        enabled: !editor.readOnly
                    }
                    AppTextField {
                        id: startTimeField
                        objectName: "eventStartTime"
                        visible: !allDay.checked
                        Layout.fillWidth: true
                        placeholderText: qsTr("09:00")
                        accessibleName: qsTr("Start time")
                        enabled: !editor.readOnly
                    }
                    Item { visible: allDay.checked; Layout.fillWidth: true }
                    Item { Layout.preferredWidth: 1 }

                    Text {
                        textFormat: Text.PlainText
                        text: qsTr("Ends")
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                    }
                    AppTextField {
                        id: endDateField
                        objectName: "eventEndDate"
                        Layout.fillWidth: true
                        placeholderText: qsTr("YYYY-MM-DD")
                        accessibleName: qsTr("End date")
                        enabled: !editor.readOnly
                    }
                    AppTextField {
                        id: endTimeField
                        objectName: "eventEndTime"
                        visible: !allDay.checked
                        Layout.fillWidth: true
                        placeholderText: qsTr("10:00")
                        accessibleName: qsTr("End time")
                        enabled: !editor.readOnly
                    }
                    Item { visible: allDay.checked; Layout.fillWidth: true }
                    Item { Layout.preferredWidth: 1 }
                }
                RowLayout {
                    visible: !allDay.checked
                    Layout.fillWidth: true
                    AppComboBox {
                        id: timeKindBox
                        Layout.preferredWidth: 145
                        model: [
                            {"text": qsTr("Zoned time"), "value": "zoned"},
                            {"text": qsTr("Floating time"), "value": "floating"}
                        ]
                        textRole: "text"
                        valueRole: "value"
                        enabled: !editor.readOnly
                        Accessible.name: qsTr("Event time type")
                        ToolTip.visible: hovered
                        ToolTip.text: currentValue === "floating"
                                      ? qsTr("Keeps the same wall-clock time in every time zone")
                                      : qsTr("Keeps the event at one absolute moment")
                    }
                    AppComboBox {
                        id: timeZoneBox
                        objectName: "eventTimeZone"
                        visible: timeKindBox.currentValue !== "floating"
                        Layout.fillWidth: true
                        model: App.availableTimeZoneIds
                        Accessible.name: qsTr("Time zone")
                        enabled: !editor.readOnly
                    }
                }

                SectionLabel { text: qsTr("DETAILS") }
                AppTextField {
                    id: locationField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Location")
                    accessibleName: qsTr("Location")
                    enabled: !editor.readOnly
                }
                RowLayout {
                    Layout.fillWidth: true
                    AppTextField {
                        id: urlField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Meeting or event URL")
                        accessibleName: qsTr("Meeting or event URL")
                        enabled: !editor.readOnly
                        inputMethodHints: Qt.ImhUrlCharactersOnly
                    }
                    AppButton {
                        visible: urlField.text.trim().length > 0
                        text: qsTr("Join")
                        compact: true
                        onClicked: editor.joinRequested(urlField.text.trim())
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: editor.conferenceUrl.length > 0
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Video call: %1").arg(editor.conferenceUrl)
                        textFormat: Text.PlainText
                        color: Theme.mutedText
                        elide: Text.ElideMiddle
                    }
                    AppButton {
                        objectName: "joinConferenceButton"
                        text: qsTr("Join")
                        compact: true
                        onClicked: editor.joinRequested(editor.conferenceUrl)
                    }
                }
                AppCheckBox {
                    id: meetBox
                    objectName: "addMeetCheckBox"
                    visible: editor.meetSupported && editor.conferenceUrl.length === 0
                             && !editor.readOnly && !editor.movingCalendars
                    text: qsTr("Add Google Meet video conferencing")
                    Accessible.name: text
                }
                TextArea {
                    id: notesField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    placeholderText: qsTr("Notes")
                    color: Theme.text
                    placeholderTextColor: Theme.mutedText
                    enabled: !editor.readOnly
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    Accessible.name: qsTr("Event notes")
                    background: Rectangle {
                        radius: Theme.radiusMD
                        color: Theme.background
                        border.color: notesField.activeFocus ? Theme.focus : Theme.border
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: editor.attachments.length > 0
                    spacing: 4
                    SectionLabel { text: qsTr("ATTACHMENTS") }
                    Repeater {
                        model: editor.attachments
                        delegate: RowLayout {
                            id: attachmentRow
                            required property var modelData
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: String(attachmentRow.modelData.title || "")
                                textFormat: Text.PlainText
                                color: Theme.text
                                elide: Text.ElideMiddle
                            }
                            AppButton {
                                text: qsTr("Open")
                                compact: true
                                Accessible.name: qsTr("Open %1")
                                                 .arg(String(attachmentRow.modelData.title || ""))
                                onClicked: editor.joinRequested(
                                               String(attachmentRow.modelData.url || ""))
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    AppComboBox {
                        id: availabilityBox
                        Layout.fillWidth: true
                        model: [
                            {"text": qsTr("Busy"), "value": "busy"},
                            {"text": qsTr("Free"), "value": "free"}
                        ]
                        textRole: "text"
                        valueRole: "value"
                        enabled: !editor.readOnly
                        Accessible.name: qsTr("Availability")
                    }
                    AppComboBox {
                        id: visibilityBox
                        Layout.fillWidth: true
                        model: [
                            {"text": qsTr("Default visibility"), "value": "default"},
                            {"text": qsTr("Public"), "value": "public"},
                            {"text": qsTr("Private"), "value": "private"},
                            {"text": qsTr("Confidential"), "value": "confidential"}
                        ]
                        textRole: "text"
                        valueRole: "value"
                        enabled: !editor.readOnly
                        Accessible.name: qsTr("Visibility")
                    }
                }

                SectionLabel { text: qsTr("REPEAT") }
                RecurrenceEditor {
                    id: recurrenceEditor
                    Layout.fillWidth: true
                    startDate: {
                        const parsed = new Date(startDateField.text + "T00:00:00")
                        return isNaN(parsed.getTime()) ? new Date() : parsed
                    }
                    allDayEvent: allDay.checked
                    floating: timeKindBox.currentValue === "floating"
                    timeZone: editor.selectedTimeZone()
                    editable: !editor.readOnly && editor.recurrenceEditingSupported
                }
                AppComboBox {
                    id: scopeBox
                    objectName: "eventRecurrenceScope"
                    visible: editor.editing && editor.recurring
                    Layout.fillWidth: true
                    model: editor.futureScopeSupported
                           ? [
                               {"text": qsTr("Choose recurrence scope…"), "value": ""},
                               {"text": qsTr("This occurrence"), "value": "occurrence"},
                               {"text": qsTr("This and future occurrences"), "value": "future"},
                               {"text": qsTr("Entire series"), "value": "series"}
                           ] : [
                               {"text": qsTr("Choose recurrence scope…"), "value": ""},
                               {"text": qsTr("This occurrence"), "value": "occurrence"},
                               {"text": qsTr("Entire series"), "value": "series"}
                           ]
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Recurring event edit scope")
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "futureSupportMessage"
                    visible: editor.editing && editor.recurring
                             && !editor.futureScopeSupported
                    Layout.fillWidth: true
                    text: editor.movingCalendars
                          ? qsTr("This and future occurrences cannot be moved between calendars. Choose this occurrence or the entire series.")
                          : editor.activeCapabilities.thisAndFutureProbeState === "failed"
                            ? qsTr("Support check failed: %1. This and future changes remain disabled.")
                                .arg(editor.activeCapabilities.thisAndFutureProbeMessage || qsTr("The server could not be verified"))
                            : editor.futureScopeCheckAvailable
                              ? qsTr("Check server support to enable this and future changes. The check creates and removes a temporary test event; your events are unchanged.")
                              : qsTr("This calendar does not support changing this and future occurrences.")
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }
                AppButton {
                    objectName: "checkFutureSupport"
                    visible: editor.editing && editor.recurring
                             && !editor.futureScopeSupported
                             && editor.futureScopeCheckAvailable
                    text: qsTr("Check this-and-future support")
                    enabled: App.connected && !App.busy
                    onClicked: App.probeThisAndFuture(String(editor.activeCalendar.id || ""))
                }
                Text {
                    textFormat: Text.PlainText
                    visible: !editor.readOnly && !editor.recurrenceEditingSupported
                    Layout.fillWidth: true
                    text: qsTr("This calendar cannot write recurring events. Existing recurrence data is preserved.")
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }

                SectionLabel { text: qsTr("GUESTS") }
                AttendeeEditor {
                    id: attendeeEditor
                    Layout.fillWidth: true
                    editable: !editor.readOnly && editor.attendeeEditingSupported
                }
                AvailabilityStrip {
                    id: availability
                    objectName: "availabilityStrip"
                    visible: editor.showAvailability
                    Layout.fillWidth: true
                    freeBusy: App.freeBusy || ({})
                    guests: editor.guestEmails
                    dateText: startDateField.text
                    startText: startTimeField.text
                    endText: endDateField.text === startDateField.text ? endTimeField.text
                                                                       : "24:00"
                    timeZone: editor.selectedTimeZone()
                    workDayStart: editor.workDayStart
                    workDayEnd: editor.workDayEnd
                    onSlotRequested: editor.findFreeTime()
                }
                Text {
                    objectName: "freeTimeMessage"
                    visible: editor.showAvailability && editor.freeTimeMessage.length > 0
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: editor.freeTimeMessage
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }
                AppComboBox {
                    id: notificationBox
                    visible: editor.hasGuests && editor.attendeeEditingSupported
                    Layout.fillWidth: true
                    model: [
                        {"text": qsTr("Choose guest notification policy…"), "value": ""},
                        {"text": qsTr("Do not notify guests"), "value": "none"},
                        {"text": qsTr("Notify external guests only"), "value": "externalOnly"},
                        {"text": qsTr("Notify all guests"), "value": "all"}
                    ]
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Guest notification policy")
                }
                Text {
                    textFormat: Text.PlainText
                    visible: !editor.readOnly && !editor.attendeeEditingSupported
                    Layout.fillWidth: true
                    text: editor.activeProvider === "caldav"
                          ? qsTr("This CalDAV server did not advertise scheduling, so guest and RSVP writes are disabled. Existing attendee data is preserved.")
                          : qsTr("This calendar does not support guest or invitation writes. Existing attendee data is preserved.")
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    SectionLabel {
                        Layout.fillWidth: true
                        text: qsTr("REMINDERS")
                    }
                    AppButton {
                        iconText: "+"
                        text: qsTr("Add")
                        compact: true
                        quiet: true
                        enabled: !editor.readOnly && editor.reminderEditingSupported
                        onClicked: reminderModel.append({
                            "kind": "relative", "minutes": 10,
                            "method": "popup", "providerDefault": false,
                            "absoluteAt": "", "sourceIndex": -1,
                            "edited": true
                        })
                    }
                }
                Repeater {
                    model: ListModel { id: reminderModel }
                    delegate: RowLayout {
                        id: reminderRow
                        required property int index
                        required property int minutes
                        required property string method
                        required property bool providerDefault
                        required property string kind
                        required property string absoluteAt
                        required property int sourceIndex
                        required property bool edited
                        Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                textFormat: Text.PlainText
                                objectName: "reminderLabel-" + reminderRow.index
                                Layout.fillWidth: true
                                text: reminderRow.kind === "absolute"
                                      ? editor.absoluteReminderText(
                                            reminderRow.method,
                                            reminderRow.absoluteAt)
                                      : reminderRow.kind === "unsupported"
                                        ? qsTr("Provider reminder")
                                        : reminderRow.minutes === 0
                                          ? qsTr("At start time")
                                          : reminderRow.minutes < 60
                                            ? reminderRow.minutes + qsTr(" minutes before")
                                            : (reminderRow.minutes / 60)
                                              + qsTr(" hours before")
                                color: Theme.text
                                font.pixelSize: Theme.smallFontSize
                                wrapMode: Text.Wrap
                                Accessible.name: text
                            }
                            Text {
                                textFormat: Text.PlainText
                                visible: reminderRow.kind !== "relative"
                                Layout.fillWidth: true
                                text: reminderRow.kind === "absolute"
                                      ? qsTr("This absolute reminder is read-only and will be preserved. Remove it to discard it.")
                                      : qsTr("This provider reminder format is read-only and will be preserved. Remove it to discard it.")
                                color: Theme.mutedText
                                font.pixelSize: Theme.smallFontSize
                                wrapMode: Text.Wrap
                            }
                        }
                        AppComboBox {
                            objectName: "relativeReminderEditor-" + reminderRow.index
                            visible: reminderRow.kind === "relative"
                            model: [0, 5, 10, 15, 30, 60, 120, 1440]
                            currentIndex: Math.max(0, model.indexOf(reminderRow.minutes))
                            enabled: !editor.readOnly && editor.reminderEditingSupported
                            delegate: ItemDelegate {
                                required property var modelData
                                width: ListView.view.width
                                text: modelData === 0 ? qsTr("At start time")
                                                     : modelData < 60 ? modelData + qsTr(" minutes")
                                                                      : modelData < 1440
                                                                        ? (modelData / 60) + qsTr(" hours")
                                                                        : qsTr("1 day")
                            }
                            onActivated: index => {
                                reminderModel.setProperty(reminderRow.index,
                                                          "minutes", model[index])
                                reminderModel.setProperty(reminderRow.index,
                                                          "providerDefault", false)
                                reminderModel.setProperty(reminderRow.index,
                                                          "edited", true)
                            }
                            Accessible.name: qsTr("Reminder time")
                        }
                        AppButton {
                            objectName: "removeReminder-" + reminderRow.index
                            iconText: "×"
                            compact: true
                            quiet: true
                            destructive: true
                            enabled: !editor.readOnly && editor.reminderEditingSupported
                            toolTipText: qsTr("Remove reminder")
                            onClicked: reminderModel.remove(reminderRow.index)
                        }
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    visible: !editor.readOnly && !editor.reminderEditingSupported
                    Layout.fillWidth: true
                    text: qsTr("This calendar cannot write reminders. Existing provider reminders are preserved.")
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }

                Text {
                    textFormat: Text.PlainText
                    visible: editor.validationError.length > 0
                    Layout.fillWidth: true
                    text: editor.validationError
                    color: Theme.danger
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }
                Item { Layout.preferredHeight: 8 }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.divider
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingMD
            spacing: Theme.spacingSM

            AppButton {
                visible: editor.editing && !editor.readOnly
                text: editor.deleteArmed ? qsTr("Confirm delete") : qsTr("Delete")
                destructive: true
                onClicked: editor.requestDelete()
            }
            AppButton {
                visible: editor.editing
                text: qsTr("Duplicate")
                quiet: true
                onClicked: editor.duplicateRequested(editor.eventData)
            }
            AppButton {
                visible: editor.editing
                text: qsTr("Export .ics")
                quiet: true
                onClicked: editor.exportRequested(String(editor.eventData.id))
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                quiet: true
                onClicked: editor.close()
            }
            AppButton {
                text: editor.editing ? qsTr("Save changes") : qsTr("Add event")
                primary: true
                enabled: !editor.readOnly
                         && titleField.text.trim().length > 0
                         && calendarBox.currentIndex >= 0
                onClicked: editor.submit()
            }
        }
    }

    Timer {
        id: deleteReset
        interval: 4000
        repeat: false
        onTriggered: editor.deleteArmed = false
    }
}
