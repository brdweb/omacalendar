.pragma library

function startOfWeek(dateValue, firstDayOfWeek) {
    const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                           dateValue.getDate())
    const jsFirstDay = firstDayOfWeek === 7 ? 0 : firstDayOfWeek
    const distance = (start.getDay() - jsFirstDay + 7) % 7
    start.setDate(start.getDate() - distance)
    return start
}

function includeMonthGrid(rangeStart, rangeEnd, monthDate, firstDayOfWeek) {
    const monthStart = new Date(monthDate.getFullYear(), monthDate.getMonth(), 1)
    const gridStart = startOfWeek(monthStart, firstDayOfWeek)
    const gridEnd = new Date(gridStart.getFullYear(), gridStart.getMonth(),
                             gridStart.getDate() + 41)
    return {
        "start": gridStart < rangeStart ? gridStart : rangeStart,
        "end": gridEnd > rangeEnd ? gridEnd : rangeEnd
    }
}

// ISO 8601 week number: weeks start on Monday and week 1 holds the year's
// first Thursday.
function isoWeekNumber(dateValue) {
    const day = new Date(Date.UTC(dateValue.getFullYear(), dateValue.getMonth(),
                                  dateValue.getDate()))
    const weekday = day.getUTCDay() || 7
    day.setUTCDate(day.getUTCDate() + 4 - weekday)
    const yearStart = new Date(Date.UTC(day.getUTCFullYear(), 0, 1))
    return Math.ceil(((day - yearStart) / 86400000 + 1) / 7)
}

// The ISO week shown for a seven-day row that starts on rowStart. The row's
// Thursday decides, so Sunday-first rows label the week most of them share.
function rowWeekNumber(rowStart) {
    const thursday = new Date(rowStart.getFullYear(), rowStart.getMonth(),
                              rowStart.getDate() + (4 - rowStart.getDay() + 7) % 7)
    return isoWeekNumber(thursday)
}

function addDays(dateValue, days) {
    return new Date(dateValue.getFullYear(), dateValue.getMonth(),
                    dateValue.getDate() + days)
}

function isWeekend(dateValue) {
    const weekday = dateValue.getDay()
    return weekday === 0 || weekday === 6
}

// The week view's length: 7 is a week aligned to the first day of the week,
// 2 to 6 a run of days from the selected date. Anything else falls back to 7.
function weekViewDayCount(value) {
    const count = Math.round(Number(value))
    return count >= 2 && count <= 7 ? count : 7
}

// The days the week view shows for anchor. A full week holds anchor and
// drops Saturday and Sunday when hideWeekends is set; a shorter run starts on
// anchor (or the next weekday) and skips weekends without getting shorter.
function visibleDays(anchor, firstDayOfWeek, dayCount, hideWeekends) {
    const count = weekViewDayCount(dayCount)
    const days = []
    if (count === 7) {
        const start = startOfWeek(anchor, firstDayOfWeek)
        for (let offset = 0; offset < 7; ++offset) {
            const day = addDays(start, offset)
            if (!hideWeekends || !isWeekend(day))
                days.push(day)
        }
        return days
    }
    let day = addDays(anchor, 0)
    while (days.length < count) {
        if (!hideWeekends || !isWeekend(day))
            days.push(day)
        day = addDays(day, 1)
    }
    return days
}

// The anchor of the period before (direction -1) or after (+1) the one
// visibleDays shows for anchor. A full week keeps the weekday; a run of days
// moves by its own length, counting only weekdays when weekends are hidden.
function shiftAnchor(anchor, firstDayOfWeek, dayCount, hideWeekends, direction) {
    const count = weekViewDayCount(dayCount)
    const step = direction < 0 ? -1 : 1
    if (count === 7)
        return addDays(anchor, 7 * step)
    if (!hideWeekends)
        return addDays(anchor, count * step)
    const shown = visibleDays(anchor, firstDayOfWeek, count, true)
    if (step > 0)
        return addDays(shown[shown.length - 1], 1)
    let day = shown[0]
    let found = 0
    while (found < count) {
        day = addDays(day, -1)
        if (!isWeekend(day))
            ++found
    }
    return day
}

// Offsets, within a seven-day row starting on rowStart, of the columns a
// month grid shows.
function rowColumnOffsets(rowStart, hideWeekends) {
    const offsets = []
    for (let offset = 0; offset < 7; ++offset) {
        if (!hideWeekends || !isWeekend(addDays(rowStart, offset)))
            offsets.push(offset)
    }
    return offsets
}
