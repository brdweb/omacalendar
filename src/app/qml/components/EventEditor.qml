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
    property string defaultCalendarId: ""
    readonly property var sourceCalendar: calendarForId(eventData.calendarId || "")
    readonly property bool readOnly: Boolean(eventData && eventData.readOnly)
                                     || sourceCalendar.readOnly === true
    readonly property bool recurring: Boolean(eventData && (eventData.recurrenceRule
                                                              || eventData.recurrenceId))
    readonly property bool hasGuests: attendeeField.text.trim().length > 0
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
        attendeeField.text = ""
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
        recurrenceBox.currentIndex = 0
        recurrenceRuleField.text = ""
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

    function openExisting(value) {
        eventData = value || ({})
        editing = Boolean(eventData.id)
        deleteArmed = false
        validationError = ""
        titleField.text = eventData.summary || ""
        locationField.text = eventData.location || ""
        urlField.text = eventData.url || eventData.meetingUrl || ""
        notesField.text = eventData.description || ""
        attendeeField.text = attendeeText(eventData.attendees || [])
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
        configureRecurrence(eventData.recurrenceRule || "")
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

    function configureRecurrence(rule) {
        const normalized = String(rule).toUpperCase()
        if (!normalized) {
            recurrenceBox.currentIndex = 0
            recurrenceRuleField.text = ""
        } else if (normalized.indexOf("FREQ=DAILY") >= 0) {
            recurrenceBox.currentIndex = 1
            recurrenceRuleField.text = rule
        } else if (normalized.indexOf("FREQ=WEEKLY") >= 0) {
            recurrenceBox.currentIndex = 2
            recurrenceRuleField.text = rule
        } else if (normalized.indexOf("FREQ=MONTHLY") >= 0) {
            recurrenceBox.currentIndex = 3
            recurrenceRuleField.text = rule
        } else if (normalized.indexOf("FREQ=YEARLY") >= 0) {
            recurrenceBox.currentIndex = 4
            recurrenceRuleField.text = rule
        } else {
            recurrenceBox.currentIndex = 5
            recurrenceRuleField.text = rule
        }
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
                validationError = "That wall time does not exist in the selected time zone."
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
            return "This event belongs to a read-only calendar."
        if (!titleField.text.trim())
            return "Add an event title."
        if (calendarBox.currentIndex < 0)
            return "Choose a writable calendar."
        const startDate = new Date(startDateField.text + "T00:00:00")
        const endDate = new Date(endDateField.text + "T00:00:00")
        if (isNaN(startDate.getTime()) || isNaN(endDate.getTime()))
            return "Enter valid start and end dates."
        if (allDay.checked) {
            if (endDate < startDate)
                return "The end date must be on or after the start date."
        } else {
            const start = dateTime(startDateField.text, startTimeField.text)
            const end = dateTime(endDateField.text, endTimeField.text)
            if (isNaN(start.getTime()) || isNaN(end.getTime()))
                return "Enter valid start and end times."
            if (end <= start)
                return "The event must end after it starts."
            if (timeKindBox.currentValue !== "floating"
                    && !App.isValidTimeZone(selectedTimeZone()))
                return "Enter a valid IANA time zone."
        }
        if (recurrenceBox.currentIndex === 5
                && recurrenceRuleField.text.trim().length === 0)
            return "Enter a recurrence rule for the custom repeat option."
        if (recurrenceBox.currentIndex > 0 && !recurrenceEditingSupported)
            return "This calendar cannot write recurring events."
        if (hasGuests && !attendeeEditingSupported)
            return "This calendar cannot write guests or invitations."
        if (editing && recurring && !scopeBox.currentValue)
            return "Choose which recurring events this change applies to."
        if (hasGuests && !notificationBox.currentValue)
            return "Choose whether guests should be notified."
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
        if (recurrenceBox.currentIndex === 0)
            return ""
        if (recurrenceBox.currentIndex === 1)
            return "FREQ=DAILY"
        if (recurrenceBox.currentIndex === 2)
            return "FREQ=WEEKLY"
        if (recurrenceBox.currentIndex === 3)
            return "FREQ=MONTHLY"
        if (recurrenceBox.currentIndex === 4)
            return "FREQ=YEARLY"
        return recurrenceRuleField.text.trim().replace(/^RRULE:/i, "")
    }

    function parsedAttendees() {
        const values = attendeeField.text.split(/[\n,;]/)
        const result = []
        const existing = eventData.attendees || []
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
            return "Email"
        if (normalized === "audio")
            return "Audio"
        return "Popup"
    }

    function absoluteReminderText(method, value) {
        const parsed = new Date(value)
        const timestamp = isNaN(parsed.getTime())
                ? String(value) : Qt.formatDateTime(parsed, Locale.ShortFormat)
        return reminderMethodLabel(method) + " at " + timestamp
    }

    function attendeeText(values) {
        const result = []
        for (let index = 0; index < values.length; ++index) {
            if (values[index].email)
                result.push(values[index].email)
        }
        return result.join(", ")
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
        radius: 18
        color: Theme.surface
        border.color: Theme.border
    }

    header: Rectangle {
        implicitHeight: 66
        color: "transparent"
        border.color: Theme.divider

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 22
            anchors.rightMargin: 16
            spacing: 10

            AppCloseButton {
                toolTipText: "Close event editor"
                onClicked: editor.close()
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    text: editor.editing ? "Event details" : "New event"
                    color: Theme.text
                    font.pixelSize: Theme.fontSize + 5
                    font.weight: Font.Bold
                }
                Text {
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
                text: editor.eventData.conflict === true ? "Conflict"
                                                         : editor.readOnly ? "Read only"
                                                                           : "Pending"
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
                spacing: 13

                Item { Layout.preferredHeight: 4 }

                Rectangle {
                    visible: editor.readOnly || editor.eventData.conflict === true
                    Layout.fillWidth: true
                    implicitHeight: stateMessage.implicitHeight + 20
                    radius: Theme.smallRadius
                    color: Theme.alpha(editor.eventData.conflict === true
                                       ? Theme.danger : Theme.info, 0.1)
                    border.color: Theme.alpha(editor.eventData.conflict === true
                                              ? Theme.danger : Theme.info, 0.28)
                    Text {
                        id: stateMessage
                        anchors.fill: parent
                        anchors.margins: 10
                        text: editor.eventData.conflict === true
                              ? "A newer local or remote update is being selected in the background."
                              : "This calendar is read-only. You can inspect details or duplicate the event into a writable calendar."
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                        wrapMode: Text.Wrap
                    }
                }

                SectionLabel { text: "EVENT" }
                AppTextField {
                    id: titleField
                    Layout.fillWidth: true
                    placeholderText: "Event title"
                    accessibleName: "Event title"
                    enabled: !editor.readOnly
                    font.pixelSize: Theme.fontSize + 2
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    AppComboBox {
                        id: calendarBox
                        objectName: "eventCalendar"
                        Layout.fillWidth: true
                        model: editor.writableCalendars
                        textRole: "name"
                        valueRole: "id"
                        enabled: !editor.readOnly
                        Accessible.name: "Calendar"
                    }
                    AppCheckBox {
                        id: allDay
                        text: "All day"
                        enabled: !editor.readOnly
                        Accessible.name: "All-day event"
                    }
                }

                SectionLabel { text: "DATE & TIME" }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: 9
                    rowSpacing: 8

                    Text {
                        text: "Starts"
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                    }
                    AppTextField {
                        id: startDateField
                        Layout.fillWidth: true
                        placeholderText: "YYYY-MM-DD"
                        accessibleName: "Start date"
                        enabled: !editor.readOnly
                    }
                    AppTextField {
                        id: startTimeField
                        visible: !allDay.checked
                        Layout.fillWidth: true
                        placeholderText: "09:00"
                        accessibleName: "Start time"
                        enabled: !editor.readOnly
                    }
                    Item { visible: allDay.checked; Layout.fillWidth: true }
                    Item { Layout.preferredWidth: 1 }

                    Text {
                        text: "Ends"
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                    }
                    AppTextField {
                        id: endDateField
                        Layout.fillWidth: true
                        placeholderText: "YYYY-MM-DD"
                        accessibleName: "End date"
                        enabled: !editor.readOnly
                    }
                    AppTextField {
                        id: endTimeField
                        visible: !allDay.checked
                        Layout.fillWidth: true
                        placeholderText: "10:00"
                        accessibleName: "End time"
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
                            {"text": "Zoned time", "value": "zoned"},
                            {"text": "Floating time", "value": "floating"}
                        ]
                        textRole: "text"
                        valueRole: "value"
                        enabled: !editor.readOnly
                        Accessible.name: "Event time type"
                        ToolTip.visible: hovered
                        ToolTip.text: currentValue === "floating"
                                      ? "Keeps the same wall-clock time in every time zone"
                                      : "Keeps the event at one absolute moment"
                    }
                    AppComboBox {
                        id: timeZoneBox
                        objectName: "eventTimeZone"
                        visible: timeKindBox.currentValue !== "floating"
                        Layout.fillWidth: true
                        model: App.availableTimeZoneIds
                        Accessible.name: "Time zone"
                        enabled: !editor.readOnly
                    }
                }

                SectionLabel { text: "DETAILS" }
                AppTextField {
                    id: locationField
                    Layout.fillWidth: true
                    placeholderText: "Location"
                    accessibleName: "Location"
                    enabled: !editor.readOnly
                }
                RowLayout {
                    Layout.fillWidth: true
                    AppTextField {
                        id: urlField
                        Layout.fillWidth: true
                        placeholderText: "Meeting or event URL"
                        accessibleName: "Meeting or event URL"
                        enabled: !editor.readOnly
                        inputMethodHints: Qt.ImhUrlCharactersOnly
                    }
                    AppButton {
                        visible: urlField.text.trim().length > 0
                        text: "Join"
                        compact: true
                        onClicked: editor.joinRequested(urlField.text.trim())
                    }
                }
                TextArea {
                    id: notesField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    placeholderText: "Notes"
                    color: Theme.text
                    placeholderTextColor: Theme.mutedText
                    enabled: !editor.readOnly
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    Accessible.name: "Event notes"
                    background: Rectangle {
                        radius: Theme.smallRadius
                        color: Theme.background
                        border.color: notesField.activeFocus ? Theme.focus : Theme.border
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    AppComboBox {
                        id: availabilityBox
                        Layout.fillWidth: true
                        model: [
                            {"text": "Busy", "value": "busy"},
                            {"text": "Free", "value": "free"}
                        ]
                        textRole: "text"
                        valueRole: "value"
                        enabled: !editor.readOnly
                        Accessible.name: "Availability"
                    }
                    AppComboBox {
                        id: visibilityBox
                        Layout.fillWidth: true
                        model: [
                            {"text": "Default visibility", "value": "default"},
                            {"text": "Public", "value": "public"},
                            {"text": "Private", "value": "private"},
                            {"text": "Confidential", "value": "confidential"}
                        ]
                        textRole: "text"
                        valueRole: "value"
                        enabled: !editor.readOnly
                        Accessible.name: "Visibility"
                    }
                }

                SectionLabel { text: "REPEAT" }
                AppComboBox {
                    id: recurrenceBox
                    Layout.fillWidth: true
                    model: ["Does not repeat", "Daily", "Weekly", "Monthly", "Yearly", "Custom rule"]
                    enabled: !editor.readOnly && editor.recurrenceEditingSupported
                    Accessible.name: "Event recurrence"
                }
                AppTextField {
                    id: recurrenceRuleField
                    visible: recurrenceBox.currentIndex === 5
                    Layout.fillWidth: true
                    placeholderText: "FREQ=WEEKLY;INTERVAL=2;BYDAY=MO"
                    accessibleName: "Custom recurrence rule"
                    enabled: !editor.readOnly
                }
                AppComboBox {
                    id: scopeBox
                    objectName: "eventRecurrenceScope"
                    visible: editor.editing && editor.recurring
                    Layout.fillWidth: true
                    model: editor.futureScopeSupported
                           ? [
                               {"text": "Choose recurrence scope…", "value": ""},
                               {"text": "This occurrence", "value": "occurrence"},
                               {"text": "This and future occurrences", "value": "future"},
                               {"text": "Entire series", "value": "series"}
                           ] : [
                               {"text": "Choose recurrence scope…", "value": ""},
                               {"text": "This occurrence", "value": "occurrence"},
                               {"text": "Entire series", "value": "series"}
                           ]
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: "Recurring event edit scope"
                }
                Text {
                    visible: editor.editing && editor.recurring
                             && !editor.futureScopeSupported
                    Layout.fillWidth: true
                    text: editor.movingCalendars
                          ? "This and future occurrences cannot be moved between calendars. Choose this occurrence or the entire series."
                          : "This calendar has not advertised safe support for changing this and future occurrences."
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }
                Text {
                    visible: !editor.readOnly && !editor.recurrenceEditingSupported
                    Layout.fillWidth: true
                    text: "This calendar cannot write recurring events. Existing recurrence data is preserved."
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }

                SectionLabel { text: "GUESTS" }
                TextArea {
                    id: attendeeField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    placeholderText: "Guest email addresses, separated by commas"
                    color: Theme.text
                    placeholderTextColor: Theme.mutedText
                    enabled: !editor.readOnly && editor.attendeeEditingSupported
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    Accessible.name: "Event guests"
                    background: Rectangle {
                        radius: Theme.smallRadius
                        color: Theme.background
                        border.color: attendeeField.activeFocus ? Theme.focus : Theme.border
                    }
                }
                AppComboBox {
                    id: notificationBox
                    visible: editor.hasGuests && editor.attendeeEditingSupported
                    Layout.fillWidth: true
                    model: [
                        {"text": "Choose guest notification policy…", "value": ""},
                        {"text": "Do not notify guests", "value": "none"},
                        {"text": "Notify external guests only", "value": "externalOnly"},
                        {"text": "Notify all guests", "value": "all"}
                    ]
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: "Guest notification policy"
                }
                Text {
                    visible: !editor.readOnly && !editor.attendeeEditingSupported
                    Layout.fillWidth: true
                    text: editor.activeProvider === "caldav"
                          ? "This CalDAV server did not advertise scheduling, so guest and RSVP writes are disabled. Existing attendee data is preserved."
                          : "This calendar does not support guest or invitation writes. Existing attendee data is preserved."
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    SectionLabel {
                        Layout.fillWidth: true
                        text: "REMINDERS"
                    }
                    AppButton {
                        iconText: "+"
                        text: "Add"
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
                                objectName: "reminderLabel-" + reminderRow.index
                                Layout.fillWidth: true
                                text: reminderRow.kind === "absolute"
                                      ? editor.absoluteReminderText(
                                            reminderRow.method,
                                            reminderRow.absoluteAt)
                                      : reminderRow.kind === "unsupported"
                                        ? "Provider reminder"
                                        : reminderRow.minutes === 0
                                          ? "At start time"
                                          : reminderRow.minutes < 60
                                            ? reminderRow.minutes + " minutes before"
                                            : (reminderRow.minutes / 60)
                                              + " hours before"
                                color: Theme.text
                                font.pixelSize: Theme.smallFontSize
                                wrapMode: Text.Wrap
                                Accessible.name: text
                            }
                            Text {
                                visible: reminderRow.kind !== "relative"
                                Layout.fillWidth: true
                                text: reminderRow.kind === "absolute"
                                      ? "This absolute reminder is read-only and will be preserved. Remove it to discard it."
                                      : "This provider reminder format is read-only and will be preserved. Remove it to discard it."
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
                                text: modelData === 0 ? "At start time"
                                                     : modelData < 60 ? modelData + " minutes"
                                                                      : modelData < 1440
                                                                        ? (modelData / 60) + " hours"
                                                                        : "1 day"
                            }
                            onActivated: index => {
                                reminderModel.setProperty(reminderRow.index,
                                                          "minutes", model[index])
                                reminderModel.setProperty(reminderRow.index,
                                                          "providerDefault", false)
                                reminderModel.setProperty(reminderRow.index,
                                                          "edited", true)
                            }
                            Accessible.name: "Reminder time"
                        }
                        AppButton {
                            objectName: "removeReminder-" + reminderRow.index
                            iconText: "×"
                            compact: true
                            quiet: true
                            destructive: true
                            enabled: !editor.readOnly && editor.reminderEditingSupported
                            toolTipText: "Remove reminder"
                            onClicked: reminderModel.remove(reminderRow.index)
                        }
                    }
                }
                Text {
                    visible: !editor.readOnly && !editor.reminderEditingSupported
                    Layout.fillWidth: true
                    text: "This calendar cannot write reminders. Existing provider reminders are preserved."
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }

                Text {
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
            Layout.margins: 14
            spacing: 8

            AppButton {
                visible: editor.editing && !editor.readOnly
                text: editor.deleteArmed ? "Confirm delete" : "Delete"
                destructive: true
                onClicked: editor.requestDelete()
            }
            AppButton {
                visible: editor.editing
                text: "Duplicate"
                quiet: true
                onClicked: editor.duplicateRequested(editor.eventData)
            }
            AppButton {
                visible: editor.editing
                text: "Export .ics"
                quiet: true
                onClicked: editor.exportRequested(String(editor.eventData.id))
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: "Cancel"
                quiet: true
                onClicked: editor.close()
            }
            AppButton {
                text: editor.editing ? "Save changes" : "Add event"
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
