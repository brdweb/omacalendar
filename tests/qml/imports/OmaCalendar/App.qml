pragma Singleton
import QtQuick

QtObject {
    readonly property string systemTimeZoneId: "America/New_York"
    readonly property var availableTimeZoneIds: ["America/New_York", "UTC",
                                                  "Europe/London"]
    readonly property bool bundledGoogleOAuthAvailable: false
    readonly property bool googleOAuthConfigured: false

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
            "id": "calendar-future-scope",
            "accountId": "account-local",
            "name": "Future scope fixture",
            "color": "#73daca",
            "enabled": true,
            "readOnly": false,
            "capabilities": {"thisAndFuture": true}
        }
    ]

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

    function isValidTimeZone(value) {
        return typeof value === "string" && value.trim().length > 0
    }

    function connectGoogleWithClientId(clientId, displayName) {
        clientId
        displayName
    }

    function connectGoogleConfigured(displayName) {
        displayName
    }
}
