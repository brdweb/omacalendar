pragma Singleton
import QtQuick

QtObject {
    id: app

    property bool connected: true
    property bool busy: false
    property string statusText: "All calendars up to date"
    property string lastError: ""
    property date selectedDate: new Date(2026, 8, 14, 12, 0, 0)
    property string activeCalendarSetId: "all-calendars"
    property bool preferencesLoaded: true
    property bool widgetInstalled: true
    property bool bundledGoogleOAuthAvailable: true
    property bool googleOAuthConfigured: true
    readonly property string systemTimeZoneId: "America/New_York"
    readonly property var availableTimeZoneIds: [
        "America/New_York", "America/Chicago", "America/Denver",
        "America/Los_Angeles", "UTC"
    ]

    readonly property var accounts: [
        {
            "id": "account-local",
            "provider": "local",
            "displayName": "On this device",
            "status": "connected"
        },
        {
            "id": "account-cloud",
            "provider": "google",
            "displayName": "Demo workspace",
            "status": "connected"
        }
    ]

    readonly property var calendars: [
        {
            "id": "calendar-focus",
            "accountId": "account-local",
            "name": "Focus",
            "color": "#7aa2f7",
            "enabled": true,
            "readOnly": false,
            "capabilities": {
                "provider": "local",
                "recurringEvents": true,
                "reminders": true,
                "thisAndFuture": true
            }
        },
        {
            "id": "calendar-team",
            "accountId": "account-cloud",
            "name": "Studio",
            "color": "#bb9af7",
            "enabled": true,
            "readOnly": false,
            "capabilities": {
                "provider": "google",
                "recurringEvents": true,
                "reminders": true,
                "attendeeWrites": true,
                "thisAndFuture": true
            }
        },
        {
            "id": "calendar-shared",
            "accountId": "account-cloud",
            "name": "Community",
            "color": "#9ece6a",
            "enabled": true,
            "readOnly": true,
            "capabilities": {
                "provider": "google",
                "recurringEvents": true,
                "reminders": false
            }
        }
    ]

    readonly property var calendarSets: [
        {
            "id": "all-calendars",
            "name": "All calendars",
            "calendarIds": ["calendar-focus", "calendar-team", "calendar-shared"]
        },
        {
            "id": "deep-work",
            "name": "Deep work",
            "calendarIds": ["calendar-focus"]
        }
    ]

    readonly property var events: [
        event("event-01", "calendar-team", "Quarter kickoff", "#bb9af7",
              "2026-09-01T14:00:00", "2026-09-01T15:00:00"),
        event("event-02", "calendar-focus", "Writing block", "#7aa2f7",
              "2026-09-03T09:00:00", "2026-09-03T11:00:00"),
        allDayEvent("event-03", "calendar-shared", "Neighborhood market",
                    "#9ece6a", "2026-09-05", "2026-09-06", true),
        event("event-04", "calendar-team", "Prototype review", "#bb9af7",
              "2026-09-08T13:30:00", "2026-09-08T14:30:00"),
        allDayEvent("event-05", "calendar-focus", "Planning day", "#7aa2f7",
                    "2026-09-10", "2026-09-11", false),
        event("event-06", "calendar-focus", "Weekly planning", "#7aa2f7",
              "2026-09-14T09:00:00", "2026-09-14T10:00:00"),
        event("event-07", "calendar-team", "Design critique", "#bb9af7",
              "2026-09-14T09:30:00", "2026-09-14T10:30:00"),
        event("event-08", "calendar-team", "Release checklist", "#bb9af7",
              "2026-09-14T11:00:00", "2026-09-14T12:00:00"),
        event("event-09", "calendar-focus", "Lunch break", "#7aa2f7",
              "2026-09-14T12:30:00", "2026-09-14T13:15:00"),
        event("event-10", "calendar-team", "Partner workshop", "#bb9af7",
              "2026-09-14T14:00:00", "2026-09-14T16:00:00"),
        allDayEvent("event-11", "calendar-shared", "Community exhibit",
                    "#9ece6a", "2026-09-15", "2026-09-16", true),
        event("event-12", "calendar-focus", "Research notes", "#7aa2f7",
              "2026-09-15T08:30:00", "2026-09-15T10:00:00"),
        event("event-13", "calendar-team", "Studio standup", "#bb9af7",
              "2026-09-15T10:30:00", "2026-09-15T11:00:00"),
        event("event-14", "calendar-team", "Roadmap workshop", "#bb9af7",
              "2026-09-16T09:00:00", "2026-09-16T11:30:00"),
        event("event-15", "calendar-focus", "Deep work", "#7aa2f7",
              "2026-09-16T13:00:00", "2026-09-16T15:00:00"),
        event("event-16", "calendar-shared", "Open studio", "#9ece6a",
              "2026-09-17T18:00:00", "2026-09-17T20:00:00", true),
        event("event-17", "calendar-team", "Launch rehearsal", "#bb9af7",
              "2026-09-18T10:00:00", "2026-09-18T11:30:00"),
        event("event-18", "calendar-focus", "Weekly review", "#7aa2f7",
              "2026-09-18T15:00:00", "2026-09-18T16:00:00"),
        allDayEvent("event-19", "calendar-focus", "Rest day", "#7aa2f7",
                    "2026-09-20", "2026-09-21", false),
        event("event-20", "calendar-team", "Iteration planning", "#bb9af7",
              "2026-09-22T10:00:00", "2026-09-22T11:00:00"),
        event("event-21", "calendar-focus", "Documentation sprint", "#7aa2f7",
              "2026-09-23T13:00:00", "2026-09-23T15:30:00"),
        event("event-22", "calendar-shared", "Gallery talk", "#9ece6a",
              "2026-09-24T19:00:00", "2026-09-24T20:00:00", true),
        event("event-23", "calendar-team", "Retrospective", "#bb9af7",
              "2026-09-25T14:00:00", "2026-09-25T15:00:00"),
        event("event-24", "calendar-focus", "October outline", "#7aa2f7",
              "2026-09-28T09:30:00", "2026-09-28T11:00:00"),
        allDayEvent("event-25", "calendar-team", "Release candidate",
                    "#bb9af7", "2026-09-30", "2026-10-01", false)
    ]

    readonly property var invitations: []
    readonly property var conflicts: []
    readonly property var operations: []
    readonly property var searchResults: []
    readonly property var accountsModel: null
    readonly property var calendarsModel: null
    readonly property var eventsModel: null
    readonly property var calendarSetsModel: null
    readonly property var invitationsModel: null
    readonly property var conflictsModel: null
    readonly property var operationsModel: null
    readonly property var searchResultsModel: null

    property var preferences: ({
        "firstDayOfWeek": 1,
        "workDayStart": 8,
        "workDayEnd": 18,
        "timeFormat": "12h",
        "displayTimeZone": "America/New_York",
        "defaultDuration": 60,
        "defaultCalendarId": "calendar-focus",
        "notificationPrivacy": "full_details",
        "currentView": "month",
        "widgetConsentDecision": "enabled"
    })

    signal openEventRequested(var eventData)
    signal createEventRequested(var draft)
    signal openIcsImportRequested(url file)
    signal openSectionRequested(string section)
    signal windowActivationRequested()
    signal icsImportPreviewReady(var preview)
    signal icsImportCompleted(var result)

    function event(id, calendarId, summary, color, startLocal, endLocal,
                   readOnly) {
        return {
            "id": id,
            "uid": id + "-synthetic",
            "calendarId": calendarId,
            "summary": summary,
            "description": summary === "Weekly planning"
                           ? "Review priorities and shape a calm week."
                           : "",
            "location": summary === "Weekly planning" ? "North studio" : "",
            "url": "",
            "calendarColor": color,
            "allDay": false,
            "timeKind": "zoned",
            "startUtc": startLocal + "Z",
            "endUtc": endLocal + "Z",
            "displayStartLocal": startLocal,
            "displayEndLocal": endLocal,
            "eventStartLocal": startLocal,
            "eventEndLocal": endLocal,
            "startTimeZone": "America/New_York",
            "endTimeZone": "America/New_York",
            "visibility": "default",
            "transparency": "opaque",
            "organizer": ({}),
            "attendees": [],
            "reminders": [{"method": "popup", "minutes": 15}],
            "localRevision": 1,
            "dirty": false,
            "conflict": false,
            "readOnly": readOnly === true,
            "operationState": "synced"
        }
    }

    function allDayEvent(id, calendarId, summary, color, startDate, endDate,
                         readOnly) {
        return {
            "id": id,
            "uid": id + "-synthetic",
            "calendarId": calendarId,
            "summary": summary,
            "description": "",
            "location": "",
            "url": "",
            "calendarColor": color,
            "allDay": true,
            "timeKind": "all_day",
            "startDate": startDate,
            "endDate": endDate,
            "startUtc": "",
            "endUtc": "",
            "displayStartLocal": startDate + "T00:00:00",
            "displayEndLocal": endDate + "T00:00:00",
            "eventStartLocal": startDate + "T00:00:00",
            "eventEndLocal": endDate + "T00:00:00",
            "startTimeZone": "",
            "endTimeZone": "",
            "visibility": "default",
            "transparency": "opaque",
            "organizer": ({}),
            "attendees": [],
            "reminders": [],
            "localRevision": 1,
            "dirty": false,
            "conflict": false,
            "readOnly": readOnly === true,
            "operationState": "synced"
        }
    }

    function setSelectedDate(value) { selectedDate = value }
    function setCurrentView(value) { preferences.currentView = value }
    function loadRange(firstDate, lastDate) { firstDate; lastDate }
    function loadRangeFor(firstDate, lastDate) { firstDate; lastDate }
    function reconnect() {}
    function syncAll() {}
    function syncAccount(accountId) { accountId }
    function searchEvents(query, filters) { query; filters }
    function createEvent(value) { value }
    function updateEvent(value) { value }
    function saveEvent(value, options) { value; options }
    function removeEvent(eventId) { eventId }
    function requestDeleteEvent(eventId, options) { eventId; options }
    function respondToInvitation(eventId, response, recurrenceScope,
                                 recurrenceId, expectedRevision) {
        eventId; response; recurrenceScope; recurrenceId; expectedRevision
    }
    function markInvitationSeen(eventId) { eventId }
    function resolveConflict(conflictId, strategy, mergedDraft) {
        conflictId; strategy; mergedDraft
    }
    function retryOperation(operationId) { operationId }
    function discardOperation(operationId) { operationId }
    function addLocalCalendar(name, color) { name; color }
    function addIcsSubscription(config) { config }
    function removeCalendar(calendarId) { calendarId }
    function removeAccount(accountId) { accountId }
    function removeAccountWithOptions(accountId, cachedData) { accountId; cachedData }
    function reauthorizeAccount(accountId) { accountId }
    function updateAccountCredentials(accountId, username, password) {
        accountId; username; password
    }
    function setCalendarPreference(calendarId, key, value) { calendarId; key; value }
    function setPreference(key, value) { preferences[key] = value }
    function setCalendarVisibility(calendarId, visible) { calendarId; visible }
    function upsertCalendarSet(value) { value }
    function removeCalendarSet(setId) { setId }
    function activateCalendarSet(setId) { activeCalendarSetId = setId }
    function undoLastMutation() {}
    function previewDiagnostics() {}
    function connectGoogle(displayName) { displayName }
    function connectGoogleConfigured(displayName) { displayName }
    function connectGoogleWithClientId(clientId, displayName) { clientId; displayName }
    function connectGoogleWithCredentials(file, displayName) { file; displayName }
    function addCalDavAccount(endpoint, username, password, displayName) {
        endpoint; username; password; displayName
    }
    function previewIcsImport(file, calendarId) { file; calendarId }
    function commitIcsImport(file, calendarId, policy) { file; calendarId; policy }
    function exportIcs(scope, destination) { scope; destination }
    function isValidTimeZone(value) { return value && value.length > 0 }
    function wallTimeToUtc(dateText, timeText, timeZone) {
        timeZone
        const value = new Date(dateText + "T" + timeText + ":00Z")
        return isNaN(value.getTime()) ? "" : value.toISOString()
    }
    function utcToWallTime(utcText, timeZone) {
        timeZone
        const value = new Date(utcText)
        return isNaN(value.getTime()) ? "" : value.toISOString().slice(0, 19)
    }
}
