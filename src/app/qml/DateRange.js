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
