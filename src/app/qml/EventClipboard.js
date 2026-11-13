.pragma library
.import "EventIndex.js" as EventIndex

// Copy and paste of events. A copy keeps what the user wrote: the title,
// place, link, notes, length, all-day-ness, time zone, availability,
// visibility, relative reminders and the repeat rule. It drops everything
// that names the original or belongs to someone else: the event and
// occurrence ids, provider ids, etags and raw payloads, the organizer and
// guests, conference data, and sync state. Pasting a repeating event starts a
// new series with the same rule on the paste date.

const kKeptFields = ["summary", "description", "location", "url", "timeKind",
                     "startTimeZone", "endTimeZone", "transparency", "visibility",
                     "recurrenceRule"]

// Relative reminders only, written in the editor's own form. An absolute
// reminder names a moment of the original event, and a provider default
// belongs to the source calendar.
function copiedReminders(values) {
    const result = []
    const list = Array.isArray(values) ? values : []
    for (let index = 0; index < list.length; ++index) {
        const value = list[index]
        let minutes = NaN
        let method = "popup"
        if (typeof value === "number") {
            minutes = value
        } else if (value && typeof value === "object") {
            if (value.providerDefault === true
                    || (typeof value.at === "string" && value.at.trim().length > 0))
                continue
            const keys = ["minutesBefore", "offsetMinutes", "minutes", "min"]
            for (let k = 0; k < keys.length; ++k) {
                if (value[keys[k]] !== undefined) {
                    minutes = Number(value[keys[k]])
                    break
                }
            }
            method = String(value.method || "popup").toLowerCase()
        }
        if (!isFinite(minutes) || minutes < 0 || Math.floor(minutes) !== minutes)
            continue
        if (["popup", "display", "email", "audio"].indexOf(method) < 0)
            continue
        result.push({"method": method, "minutes": minutes})
    }
    return result
}

// What the clipboard holds: the kept fields plus the event's place in the
// day, so a later paste does not depend on the original still existing.
// Returns null for a value that is not an event.
function copy(source) {
    if (!source || typeof source !== "object")
        return null
    const start = EventIndex.eventStart(source)
    const end = EventIndex.eventEnd(source)
    if (isNaN(start.getTime()) || isNaN(end.getTime()))
        return null
    const clip = ({})
    for (let index = 0; index < kKeptFields.length; ++index) {
        const key = kKeptFields[index]
        if (source[key] !== undefined && source[key] !== null)
            clip[key] = source[key]
    }
    clip.allDay = source.allDay === true
    // Where a paste goes by default; never copied into the new event as is.
    clip.sourceCalendarId = String(source.calendarId || "")
    clip.reminders = copiedReminders(source.reminders)
    if (clip.allDay) {
        clip.allDayDays = Math.max(1, Math.round((end - start) / 86400000))
        delete clip.startTimeZone
        delete clip.endTimeZone
        clip.timeKind = "all_day"
    } else {
        const utcLength = Date.parse(source.endUtc) - Date.parse(source.startUtc)
        clip.durationMinutes = Math.max(0, Math.round(
                                            (isFinite(utcLength) ? utcLength : end - start)
                                            / 60000))
        clip.startMinute = start.getHours() * 60 + start.getMinutes()
        if (clip.timeKind !== "floating")
            clip.timeKind = "zoned"
    }
    return clip
}

// A new event from a clipboard value, on dateValue in calendarId. A timed
// copy keeps its displayed start time and its length; startMinute, when a
// number, puts it at that time instead. toUtc(date, timeKind) turns a
// displayed wall time into a UTC ISO string, or "" when it does not exist.
// Returns null when the event cannot be placed.
function paste(clip, dateValue, calendarId, toUtc, startMinute) {
    if (!clip || !dateValue || isNaN(dateValue.getTime()) || !calendarId)
        return null
    const draft = ({})
    for (let index = 0; index < kKeptFields.length; ++index) {
        const key = kKeptFields[index]
        if (clip[key] !== undefined)
            draft[key] = clip[key]
    }
    draft.summary = String(draft.summary || "")
    draft.calendarId = String(calendarId)
    draft.allDay = clip.allDay === true
    draft.reminders = copiedReminders(clip.reminders)
    draft.attendees = []
    if (draft.allDay) {
        const days = Math.max(1, Number(clip.allDayDays) || 1)
        draft.startDate = formatDate(dateValue)
        draft.endDate = formatDate(new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                            dateValue.getDate() + days))
        draft.startUtc = ""
        draft.endUtc = ""
        draft.timeKind = "all_day"
        return draft
    }
    const minute = typeof startMinute === "number" && startMinute >= 0
            ? startMinute : Number(clip.startMinute) || 0
    const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                           dateValue.getDate(), Math.floor(minute / 60), minute % 60)
    const startUtc = toUtc(start, draft.timeKind)
    if (!startUtc)
        return null
    draft.startUtc = String(startUtc)
    draft.endUtc = new Date(Date.parse(draft.startUtc)
                            + Math.max(0, Number(clip.durationMinutes) || 0) * 60000)
            .toISOString()
    draft.startDate = ""
    draft.endDate = ""
    return draft
}

function formatDate(dateValue) {
    const month = String(dateValue.getMonth() + 1).padStart(2, "0")
    const day = String(dateValue.getDate()).padStart(2, "0")
    return dateValue.getFullYear() + "-" + month + "-" + day
}
