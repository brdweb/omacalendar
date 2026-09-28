pragma Singleton
import QtQuick

QtObject {
    readonly property string systemTimeZoneId: "America/New_York"
    readonly property var availableTimeZoneIds: ["America/New_York", "UTC",
                                                  "Europe/London"]
    readonly property bool bundledGoogleOAuthAvailable: false
    readonly property bool googleOAuthConfigured: false
    readonly property bool connected: true
    readonly property bool busy: false
    property string lastProbeCalendarId: ""

    readonly property var calendars: [
        {
            "id": "calendar-writable",
            "accountId": "account-local",
            "name": "Personal",
            "color": "#7aa2f7",
            "enabled": true,
            "readOnly": false
        },
        {
            "id": "calendar-read-only",
            "accountId": "account-ics",
            "name": "Subscribed",
            "color": "#9ece6a",
            "enabled": true,
            "readOnly": true
        },
        {
            "id": "calendar-unproven-caldav",
            "accountId": "account-caldav",
            "name": "Unproven CalDAV",
            "enabled": true,
            "readOnly": false,
            "capabilities": {"provider": "caldav", "thisAndFuture": false}
        },
        {
            "id": "calendar-failed-caldav",
            "accountId": "account-caldav",
            "name": "Unsupported CalDAV",
            "enabled": true,
            "readOnly": false,
            "capabilities": {"provider": "caldav", "thisAndFuture": false,
                "thisAndFutureProbeState": "failed",
                "thisAndFutureProbeMessage": "Server did not retain recurrence data"}
        },
        {
            "id": "calendar-future-scope",
            "accountId": "account-local",
            "name": "Future scope fixture",
            "color": "#73daca",
            "enabled": true,
            "readOnly": false,
            "capabilities": {"thisAndFuture": true}
        },
        {
            "id": "calendar-google",
            "accountId": "account-google",
            "name": "Work",
            "color": "#bb9af7",
            "enabled": true,
            "readOnly": false,
            "capabilities": {"provider": "google",
                "conferenceProperties": {"allowedConferenceSolutionTypes": ["hangoutsMeet"]}}
        }
    ]

    function probeThisAndFuture(calendarId) {
        lastProbeCalendarId = calendarId
    }

    function wallTimeToUtc(dateText, timeText, timeZone) {
        timeZone
        const value = new Date(dateText + "T" + timeText + ":00Z")
        return isNaN(value.getTime()) ? "" : value.toISOString()
    }

    function utcToWallTime(utcText, timeZone) {
        timeZone
        const value = new Date(utcText)
        if (isNaN(value.getTime()))
            return ""
        return value.toISOString().slice(0, 19)
    }

    // Free/busy: the smoke tests set freeBusy and nextFreeSlotResult and read
    // the last query back.
    property var freeBusy: ({})
    property var lastFreeBusyQuery: null
    property string nextFreeSlotResult: ""

    function queryFreeBusy(start, end, emails, calendarId, excludeEventId,
                           excludeRecurrenceId) {
        lastFreeBusyQuery = {"start": start, "end": end, "emails": emails,
                             "calendarId": calendarId, "excludeEventId": excludeEventId,
                             "excludeRecurrenceId": excludeRecurrenceId}
    }

    function nextFreeSlot(busy, earliest, durationMinutes, workDayStart, workDayEnd,
                          horizon, timeZone) {
        busy
        earliest
        durationMinutes
        workDayStart
        workDayEnd
        horizon
        timeZone
        return nextFreeSlotResult
    }

    function isValidTimeZone(value) {
        return typeof value === "string" && value.trim().length > 0
    }

    // Tasks: the smoke tests read the last call back.
    property bool tasksSupported: true
    property var taskLists: [{"id": "local-tasks", "name": "Tasks", "color": "#9ece6a",
                              "enabled": true}]
    property var tasks: []
    property var lastTaskCall: null

    function createTask(task) { lastTaskCall = {"method": "createTask", "task": task} }
    function updateTask(task) { lastTaskCall = {"method": "updateTask", "task": task} }
    function setTaskCompleted(taskId, completed) {
        lastTaskCall = {"method": "setTaskCompleted", "taskId": taskId,
                        "completed": completed}
    }
    function removeTask(taskId) { lastTaskCall = {"method": "removeTask", "taskId": taskId} }

    // Attachments: the smoke tests read the last lookup and set the answer.
    property var eventAttachments: ({})
    property var lastAttachmentLookup: null

    function loadEventAttachments(eventId, recurrenceId) {
        lastAttachmentLookup = {"eventId": eventId, "recurrenceId": recurrenceId}
        eventAttachments = {"eventId": eventId, "recurrenceId": recurrenceId,
                            "attachments": []}
    }

    property string lastOpenedUrl: ""

    function openExternalEventUrl(value) {
        lastOpenedUrl = value
    }

    function connectGoogleWithClientId(clientId, displayName) {
        clientId
        displayName
    }

    function connectGoogleConfigured(displayName) {
        displayName
    }
}
