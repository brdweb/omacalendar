.pragma library

// Structured editing for the RRULE subset the event editor can represent.
// Anything else is reported as not representable so the editor keeps the
// rule text exactly as written instead of silently simplifying it.

const kWeekdays = ["MO", "TU", "WE", "TH", "FR", "SA", "SU"]
const kFrequencies = ["DAILY", "WEEKLY", "MONTHLY", "YEARLY"]

// JavaScript's getDay() numbers Sunday 0; RRULE lists Monday first.
function weekdayCode(dateValue) {
    return kWeekdays[(dateValue.getDay() + 6) % 7]
}

function daysInMonth(dateValue) {
    return new Date(dateValue.getFullYear(), dateValue.getMonth() + 1, 0).getDate()
}

// Which occurrence of its weekday the date is within its month (1-5), and
// whether it is also the last one.
function weekdayOrdinal(dateValue) {
    return {"ordinal": Math.ceil(dateValue.getDate() / 7),
            "isLast": dateValue.getDate() + 7 > daysInMonth(dateValue),
            "weekday": weekdayCode(dateValue)}
}

function defaultSpec(frequency, startDate) {
    const start = startDate || new Date()
    return {
        "representable": true,
        "frequency": frequency || "WEEKLY",
        "interval": 1,
        "weekdays": [weekdayCode(start)],
        "monthlyMode": "monthday",
        "endMode": "never",
        "until": "",
        "count": 10
    }
}

// Parses rule text (with or without an RRULE: prefix). Returns a spec with
// representable false for empty or unsupported rules.
function parse(text, startDate) {
    let rule = String(text || "").trim()
    if (rule.toUpperCase().indexOf("RRULE:") === 0)
        rule = rule.slice(6)
    if (!rule || rule.indexOf("\n") >= 0 || rule.indexOf(":") >= 0)
        return {"representable": false}
    const parts = ({})
    const pieces = rule.split(";")
    for (let index = 0; index < pieces.length; ++index) {
        const piece = pieces[index].trim()
        if (!piece)
            continue
        const equals = piece.indexOf("=")
        if (equals <= 0)
            return {"representable": false}
        const key = piece.slice(0, equals).toUpperCase()
        if (parts[key] !== undefined)
            return {"representable": false}
        parts[key] = piece.slice(equals + 1).toUpperCase()
    }
    const frequency = parts.FREQ
    if (kFrequencies.indexOf(frequency) < 0)
        return {"representable": false}
    const spec = defaultSpec(frequency, startDate)
    const known = ["FREQ", "INTERVAL", "BYDAY", "BYMONTHDAY", "UNTIL", "COUNT", "WKST"]
    for (const key in parts) {
        if (known.indexOf(key) < 0)
            return {"representable": false}
    }
    if (parts.WKST !== undefined && parts.WKST !== "MO")
        return {"representable": false}
    if (parts.INTERVAL !== undefined) {
        const interval = Number(parts.INTERVAL)
        if (!Number.isInteger(interval) || interval < 1)
            return {"representable": false}
        spec.interval = interval
    }
    if (parts.UNTIL !== undefined && parts.COUNT !== undefined)
        return {"representable": false}
    if (parts.UNTIL !== undefined) {
        if (!/^\d{8}(T\d{6}Z?)?$/.test(parts.UNTIL))
            return {"representable": false}
        spec.endMode = "until"
        spec.until = parts.UNTIL
    } else if (parts.COUNT !== undefined) {
        const count = Number(parts.COUNT)
        if (!Number.isInteger(count) || count < 1)
            return {"representable": false}
        spec.endMode = "count"
        spec.count = count
    }

    if (frequency === "WEEKLY") {
        if (parts.BYMONTHDAY !== undefined)
            return {"representable": false}
        if (parts.BYDAY !== undefined) {
            const days = parts.BYDAY.split(",")
            for (let index = 0; index < days.length; ++index) {
                if (kWeekdays.indexOf(days[index]) < 0)
                    return {"representable": false}
            }
            spec.weekdays = kWeekdays.filter(function(day) {
                return days.indexOf(day) >= 0
            })
        }
    } else if (frequency === "MONTHLY") {
        const start = startDate || new Date()
        if (parts.BYMONTHDAY !== undefined && parts.BYDAY !== undefined)
            return {"representable": false}
        if (parts.BYMONTHDAY !== undefined) {
            if (Number(parts.BYMONTHDAY) !== start.getDate())
                return {"representable": false}
        } else if (parts.BYDAY !== undefined) {
            const match = /^(-1|[1-5])(MO|TU|WE|TH|FR|SA|SU)$/.exec(parts.BYDAY)
            const position = weekdayOrdinal(start)
            if (!match || match[2] !== position.weekday)
                return {"representable": false}
            if (match[1] === "-1" && position.isLast)
                spec.monthlyMode = "lastWeekday"
            else if (Number(match[1]) === position.ordinal)
                spec.monthlyMode = "weekday"
            else
                return {"representable": false}
        }
    } else if (parts.BYDAY !== undefined || parts.BYMONTHDAY !== undefined) {
        return {"representable": false}
    }
    return spec
}

// Builds rule text from a spec. untilValue is the UNTIL value already in the
// form the event needs (a date for all-day events, UTC for zoned ones).
function build(spec, startDate) {
    const pieces = ["FREQ=" + spec.frequency]
    if (spec.interval > 1)
        pieces.push("INTERVAL=" + spec.interval)
    if (spec.frequency === "WEEKLY") {
        const days = kWeekdays.filter(function(day) {
            return (spec.weekdays || []).indexOf(day) >= 0
        })
        const start = startDate || new Date()
        // A single weekday equal to the start's is implied by DTSTART.
        if (days.length > 0 && !(days.length === 1 && days[0] === weekdayCode(start)))
            pieces.push("BYDAY=" + days.join(","))
    } else if (spec.frequency === "MONTHLY") {
        const position = weekdayOrdinal(startDate || new Date())
        if (spec.monthlyMode === "weekday")
            pieces.push("BYDAY=" + position.ordinal + position.weekday)
        else if (spec.monthlyMode === "lastWeekday")
            pieces.push("BYDAY=-1" + position.weekday)
    }
    if (spec.endMode === "until" && spec.until)
        pieces.push("UNTIL=" + spec.until)
    else if (spec.endMode === "count" && spec.count > 0)
        pieces.push("COUNT=" + spec.count)
    return pieces.join(";")
}

function ordinalText(value) {
    return ["", qsTr("first"), qsTr("second"), qsTr("third"), qsTr("fourth"),
            qsTr("fifth")][value] || String(value)
}

function weekdayName(code) {
    const names = {"MO": qsTr("Monday"), "TU": qsTr("Tuesday"),
                   "WE": qsTr("Wednesday"), "TH": qsTr("Thursday"),
                   "FR": qsTr("Friday"), "SA": qsTr("Saturday"), "SU": qsTr("Sunday")}
    return names[code] || code
}

function joinNames(names) {
    if (names.length <= 1)
        return names.join("")
    return names.slice(0, -1).join(", ") + qsTr(" and ") + names[names.length - 1]
}

// A plain-language summary. untilLabel is the end date as the user should read
// it; the caller formats it because UNTIL may be UTC.
function describe(spec, startDate, untilLabel) {
    if (!spec || !spec.representable)
        return ""
    const start = startDate || new Date()
    const units = {"DAILY": [qsTr("day"), qsTr("days")],
                   "WEEKLY": [qsTr("week"), qsTr("weeks")],
                   "MONTHLY": [qsTr("month"), qsTr("months")],
                   "YEARLY": [qsTr("year"), qsTr("years")]}[spec.frequency]
    let text = spec.interval > 1
            ? qsTr("Every %1 %2").arg(spec.interval).arg(units[1])
            : qsTr("Every %1").arg(units[0])
    if (spec.frequency === "WEEKLY") {
        const days = kWeekdays.filter(function(day) {
            return (spec.weekdays || []).indexOf(day) >= 0
        })
        text += qsTr(" on %1").arg(joinNames(days.map(weekdayName)))
    } else if (spec.frequency === "MONTHLY") {
        const position = weekdayOrdinal(start)
        if (spec.monthlyMode === "weekday")
            text += qsTr(" on the %1 %2").arg(ordinalText(position.ordinal))
                                        .arg(weekdayName(position.weekday))
        else if (spec.monthlyMode === "lastWeekday")
            text += qsTr(" on the last %1").arg(weekdayName(position.weekday))
        else
            text += qsTr(" on day %1").arg(start.getDate())
    } else if (spec.frequency === "YEARLY") {
        text += qsTr(" on %1").arg(Qt.formatDate(start, "MMMM d"))
    }
    if (spec.endMode === "until" && untilLabel)
        text += qsTr(", until %1").arg(untilLabel)
    else if (spec.endMode === "count")
        text += spec.count === 1 ? qsTr(", once") : qsTr(", %1 times").arg(spec.count)
    return text
}
