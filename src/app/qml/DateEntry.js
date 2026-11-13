.pragma library

// Parsing for typed dates and times. The editors keep a canonical value
// (yyyy-MM-dd and 24-hour HH:mm) whatever the user typed or sees, so App
// and the save paths never deal with locale text.

function pad2(value) {
    return value < 10 ? "0" + value : String(value)
}

function isoDate(year, month, day) {
    return String(year) + "-" + pad2(month) + "-" + pad2(day)
}

function dateFromIso(text) {
    const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(text || ""))
    if (!match)
        return null
    const year = Number(match[1])
    const month = Number(match[2])
    const day = Number(match[3])
    const value = new Date(year, month - 1, day)
    if (value.getFullYear() !== year || value.getMonth() !== month - 1
            || value.getDate() !== day)
        return null
    return value
}

function formatIsoDate(value) {
    if (!value || isNaN(value.getTime()))
        return ""
    return isoDate(value.getFullYear(), value.getMonth() + 1, value.getDate())
}

// Accepts yyyy-MM-dd and the unpadded yyyy-M-d, with "-", "/" or "."
// between the parts. Returns the canonical yyyy-MM-dd, or "" when the text
// is not a real calendar date.
function parseDate(text) {
    const match = /^\s*(\d{4})[-/.](\d{1,2})[-/.](\d{1,2})\s*$/.exec(String(text || ""))
    if (!match)
        return ""
    const canonical = isoDate(Number(match[1]), Number(match[2]), Number(match[3]))
    return dateFromIso(canonical) ? canonical : ""
}

function normalizedMarker(text) {
    return String(text || "").toLowerCase().replace(/[\s.]/g, "")
}

// Which half of the day a suffix names: "am", "pm", "a", "p", "a.m.",
// or the locale's own AM/PM text. Returns "am", "pm" or "" when unknown.
function meridiem(suffix, amText, pmText) {
    const marker = normalizedMarker(suffix)
    if (marker.length === 0)
        return ""
    const localAm = normalizedMarker(amText)
    const localPm = normalizedMarker(pmText)
    if (marker === "am" || marker === "a" || (localAm.length > 0 && marker === localAm))
        return "am"
    if (marker === "pm" || marker === "p" || (localPm.length > 0 && marker === localPm))
        return "pm"
    return ""
}

// Lenient time entry. Accepts "9", "9:30", "9.30", "930", "0930", "21:00",
// "9am", "9:30 pm", "12am" (midnight), "12pm" (noon) and the locale's
// AM/PM words. Returns the canonical 24-hour "HH:mm", or "" when the text
// is not a time of day.
function parseTime(text, amText, pmText) {
    const value = String(text || "").trim()
    if (value.length === 0)
        return ""
    const match = /^(\d{1,4})(?:[:.](\d{2}))?(?::\d{2})?\s*(.*)$/.exec(value)
    if (!match)
        return ""
    let digits = match[1]
    let minutesText = match[2]
    if (minutesText === undefined) {
        if (digits.length > 2) {
            minutesText = digits.slice(-2)
            digits = digits.slice(0, -2)
        } else {
            minutesText = "0"
        }
    } else if (digits.length > 2) {
        return ""
    }
    let hour = Number(digits)
    const minute = Number(minutesText)
    if (minute > 59)
        return ""
    const suffix = match[3]
    if (suffix.length > 0) {
        const half = meridiem(suffix, amText, pmText)
        if (half.length === 0 || hour < 1 || hour > 12)
            return ""
        if (half === "am")
            hour = hour === 12 ? 0 : hour
        else
            hour = hour === 12 ? 12 : hour + 12
    } else if (hour > 23) {
        return ""
    }
    return pad2(hour) + ":" + pad2(minute)
}

// The display pattern for a stored time format pattern, without seconds:
// the editors only take minutes.
function minutePattern(pattern) {
    return String(pattern || "HH:mm").replace(/[:.]ss?/, "")
}

// The Date for a canonical HH:mm on an arbitrary fixed day, for formatting.
function timeValue(canonical) {
    const match = /^(\d{2}):(\d{2})$/.exec(String(canonical || ""))
    if (!match)
        return null
    return new Date(2000, 0, 1, Number(match[1]), Number(match[2]))
}

// The first date in a month grid that starts on firstDayOfWeek (Qt's
// numbering: Monday is 1, Sunday is 7; 0 is treated as Sunday).
function gridStart(year, month, firstDayOfWeek) {
    const first = new Date(year, month, 1)
    const jsFirstDay = firstDayOfWeek === 7 ? 0 : firstDayOfWeek
    const distance = (first.getDay() - jsFirstDay + 7) % 7
    return new Date(year, month, 1 - distance)
}

// Qt day numbers (1 = Monday ... 7 = Sunday) in column order.
function weekDayOrder(firstDayOfWeek) {
    const start = firstDayOfWeek >= 1 && firstDayOfWeek <= 7 ? firstDayOfWeek : 1
    const days = []
    for (let index = 0; index < 7; ++index)
        days.push((start - 1 + index) % 7 + 1)
    return days
}

function addDays(value, days) {
    return new Date(value.getFullYear(), value.getMonth(), value.getDate() + days)
}

// Moves by whole months, clamping the day to the target month's length.
function addMonths(value, months) {
    const target = new Date(value.getFullYear(), value.getMonth() + months, 1)
    const lastDay = new Date(target.getFullYear(), target.getMonth() + 1, 0).getDate()
    return new Date(target.getFullYear(), target.getMonth(),
                    Math.min(value.getDate(), lastDay))
}
