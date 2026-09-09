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
