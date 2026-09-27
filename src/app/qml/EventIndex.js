.pragma library

// Buckets events by the local calendar days they cover, so a view builds the
// index once per events change and each day cell looks up its own events
// instead of scanning, and re-parsing, the whole list.

// Events longer than this are not copied into every day they cover. They are
// kept in one short list and matched by overlap at lookup time, so a
// multi-year event costs one entry instead of hundreds of buckets.
const kLongEventMs = 62 * 24 * 60 * 60 * 1000

function eventStart(value) {
    return value.allDay ? new Date(value.startDate + "T00:00:00")
                        : new Date(value.displayStartLocal || value.startUtc)
}

function eventEnd(value) {
    return value.allDay ? new Date(value.endDate + "T00:00:00")
                        : new Date(value.displayEndLocal || value.endUtc)
}

function dayKey(dateValue) {
    return dateValue.getFullYear() * 10000 + (dateValue.getMonth() + 1) * 100
            + dateValue.getDate()
}

function startOfDay(dateValue) {
    return new Date(dateValue.getFullYear(), dateValue.getMonth(), dateValue.getDate())
}

// All-day events first, then by start time; the order every view presents.
function compareEntries(first, second) {
    if (first.allDay !== second.allDay)
        return first.allDay ? -1 : 1
    return first.startMs - second.startMs
}

// Returns {days: {dayKey: [entry]}, long: [entry]}. Each entry holds the
// event with its parsed start and end, so callers never parse dates again. An
// event belongs to every day it overlaps, with an exclusive end; a zero-length
// event belongs to the day it starts on, matching the views' overlap test.
function build(values) {
    const days = ({})
    const longEntries = []
    const list = values || []
    for (let index = 0; index < list.length; ++index) {
        const value = list[index]
        const start = eventStart(value)
        const end = eventEnd(value)
        const startMs = start.getTime()
        const endMs = end.getTime()
        if (isNaN(startMs) || isNaN(endMs))
            continue
        const entry = {"event": value, "allDay": value.allDay === true,
                       "start": start, "end": end,
                       "startMs": startMs, "endMs": endMs}
        let day = startOfDay(start)
        if (endMs <= startMs) {
            if (endMs >= day.getTime())
                appendEntry(days, dayKey(day), entry)
            continue
        }
        if (endMs - startMs > kLongEventMs) {
            longEntries.push(entry)
            continue
        }
        while (day.getTime() < endMs) {
            appendEntry(days, dayKey(day), entry)
            day = new Date(day.getFullYear(), day.getMonth(), day.getDate() + 1)
        }
    }
    for (const key in days)
        days[key].sort(compareEntries)
    return {"days": days, "long": longEntries}
}

function appendEntry(days, key, entry) {
    const bucket = days[key]
    if (bucket)
        bucket.push(entry)
    else
        days[key] = [entry]
}

// The sorted entries overlapping dateValue's local day.
function entriesForDate(index, dateValue) {
    if (!index || !dateValue)
        return []
    const bucket = index.days[dayKey(dateValue)] || []
    const longEntries = index.long || []
    if (longEntries.length === 0)
        return bucket
    const dayStart = startOfDay(dateValue)
    const dayStartMs = dayStart.getTime()
    const dayEndMs = new Date(dayStart.getFullYear(), dayStart.getMonth(),
                              dayStart.getDate() + 1).getTime()
    let merged = null
    for (let position = 0; position < longEntries.length; ++position) {
        const entry = longEntries[position]
        if (entry.startMs < dayEndMs && entry.endMs > dayStartMs) {
            if (merged === null)
                merged = bucket.slice()
            merged.push(entry)
        }
    }
    if (merged === null)
        return bucket
    return merged.sort(compareEntries)
}

// The sorted events overlapping dateValue's local day.
function eventsForDate(index, dateValue) {
    const entries = entriesForDate(index, dateValue)
    const result = []
    for (let position = 0; position < entries.length; ++position)
        result.push(entries[position].event)
    return result
}

function countForDate(index, dateValue) {
    return entriesForDate(index, dateValue).length
}
