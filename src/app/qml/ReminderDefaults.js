.pragma library

// The reminder a new event starts with. Preferences:
//   defaultReminderMinutes        timed events, default 15
//   defaultAllDayReminderMinutes  all-day events, default 15
//   calendarDefaultReminders      {calendarId: minutes or "none"}, which
//                                 replaces both for that calendar
// Each value is minutes before the start, or "none" for no reminder.

const kNone = -1
const kFallbackMinutes = 15
// The offsets the editor's reminder list offers.
const kChoices = [0, 5, 10, 15, 30, 60, 120, 1440]

// Minutes, or kNone; undefined, null and unreadable values give fallback.
function normalize(value, fallback) {
    if (value === "none")
        return kNone
    if (value === undefined || value === null || value === "")
        return fallback
    const minutes = Number(value)
    return isFinite(minutes) && minutes >= 0 ? Math.round(minutes) : fallback
}

function overrideFor(overrides, calendarId) {
    if (!overrides || typeof overrides !== "object" || Array.isArray(overrides)
            || !calendarId)
        return undefined
    return Object.prototype.hasOwnProperty.call(overrides, calendarId)
            ? overrides[calendarId] : undefined
}

// Minutes before the start for a new event, or kNone.
function resolve(preferences, calendarId, allDay) {
    const values = preferences || ({})
    const global = allDay
            ? normalize(values.defaultAllDayReminderMinutes, kFallbackMinutes)
            : normalize(values.defaultReminderMinutes, kFallbackMinutes)
    return normalize(overrideFor(values.calendarDefaultReminders, String(calendarId || "")),
                     global)
}

// The calendarDefaultReminders map with calendarId set to value (minutes or
// "none"), or removed when value is undefined or null.
function withOverride(overrides, calendarId, value) {
    const result = ({})
    if (overrides && typeof overrides === "object" && !Array.isArray(overrides)) {
        for (const key in overrides)
            result[key] = overrides[key]
    }
    if (value === undefined || value === null)
        delete result[calendarId]
    else
        result[calendarId] = value
    return result
}
