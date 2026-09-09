.pragma library

function activeSetCalendarIds(calendarSets, activeSetId) {
    const activeId = String(activeSetId || "")
    if (!activeId || activeId === "all-calendars")
        return []
    for (let index = 0; index < calendarSets.length; ++index) {
        const calendarSet = calendarSets[index]
        if (String(calendarSet.id) === activeId)
            return (calendarSet.calendarIds || []).map(String)
    }
    return []
}

function calendarIsVisible(calendars, visibilityOverrides, calendarId) {
    const id = String(calendarId || "")
    if (Object.prototype.hasOwnProperty.call(visibilityOverrides || {}, id))
        return visibilityOverrides[id] === true
    for (let index = 0; index < calendars.length; ++index) {
        const calendar = calendars[index]
        if (String(calendar.id) === id)
            return calendar.enabled !== false
    }
    return false
}

function calendarIsInActiveSet(calendarSets, activeSetId, calendarId) {
    const allowed = activeSetCalendarIds(calendarSets, activeSetId)
    return allowed.length === 0 || allowed.indexOf(String(calendarId)) >= 0
}

function calendarsForSidebar(calendars, calendarSets, activeSetId,
                             visibilityOverrides) {
    return calendars.filter(function(calendar) {
        return calendarIsVisible(calendars, visibilityOverrides, calendar.id)
                && calendarIsInActiveSet(calendarSets, activeSetId, calendar.id)
    })
}

function filterEvents(events, calendars, calendarSets, activeSetId,
                      visibilityOverrides) {
    return events.filter(function(event) {
        return calendarIsVisible(calendars, visibilityOverrides,
                                 event.calendarId)
                && calendarIsInActiveSet(calendarSets, activeSetId,
                                         event.calendarId)
    })
}
