pragma ComponentBehavior: Bound
// App is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick
import QtQuick.Layouts
import OmaCalendar

// Who is busy on the event's day: one bar per person across the working
// hours, busy time filled, the proposed time outlined. Times are shown in the
// event's own time zone.
ColumnLayout {
    id: root

    // From App.freeBusy.
    property var freeBusy: ({})
    property var guests: []
    // yyyy-MM-dd and HH:mm wall times of the proposed event, in timeZone.
    property string dateText: ""
    property string startText: ""
    property string endText: ""
    property string timeZone: ""
    property int workDayStart: 8
    property int workDayEnd: 18
    readonly property var rows: buildRows()
    readonly property bool anyConflict: rows.some(function(row) { return row.conflict })
    // Unknown availability is not free: finding a time waits for every answer.
    readonly property bool anyPending: rows.some(function(row) {
        return row.state === "pending"
    })
    signal slotRequested()

    spacing: Theme.spacingXS

    function wallMinutes(isoUtc) {
        const wall = typeof App.utcToWallTime === "function"
                ? String(App.utcToWallTime(isoUtc, timeZone)) : ""
        if (wall.length < 16)
            return null
        const day = wall.slice(0, 10)
        const minutes = Number(wall.slice(11, 13)) * 60 + Number(wall.slice(14, 16))
        // Minutes from the start of the event's day, so spans from the day
        // before start negative and spans into the next day run past 1440.
        const offsetDays = Math.round((new Date(day + "T00:00:00")
                                       - new Date(dateText + "T00:00:00")) / 86400000)
        return offsetDays * 1440 + minutes
    }

    function textMinutes(text) {
        const parts = String(text).split(":")
        return parts.length >= 2 ? Number(parts[0]) * 60 + Number(parts[1]) : NaN
    }

    // Busy spans as {from, to} minutes of the event's day.
    function spans(busy) {
        const result = []
        for (const interval of busy || []) {
            const from = wallMinutes(interval.start)
            const to = wallMinutes(interval.end)
            if (from === null || to === null || to <= 0 || from >= 1440)
                continue
            result.push({"from": Math.max(0, from), "to": Math.min(1440, to)})
        }
        return result
    }

    function overlaps(dayspans) {
        const from = textMinutes(startText)
        let to = textMinutes(endText)
        if (isNaN(from) || isNaN(to))
            return false
        if (to <= from)
            to = 1440
        return dayspans.some(function(span) { return span.from < to && span.to > from })
    }

    function unavailableReason(email) {
        for (const entry of freeBusy.unavailable || []) {
            if (String(entry.email).toLowerCase() === email)
                return String(entry.reason || "")
        }
        return ""
    }

    function buildRows() {
        if (!freeBusy || !freeBusy.requestId)
            return []
        const result = []
        const selfSpans = spans(freeBusy.self)
        result.push({"label": qsTr("You"), "state": "known", "spans": selfSpans,
                     "conflict": overlaps(selfSpans), "reason": ""})
        const attendees = freeBusy.attendees || ({})
        const pending = (freeBusy.pending || []).map(function(email) {
            return String(email).toLowerCase()
        })
        for (const guest of guests) {
            const email = String(guest).toLowerCase()
            if (attendees[email] !== undefined) {
                const guestSpans = spans(attendees[email])
                result.push({"label": guest, "state": "known", "spans": guestSpans,
                             "conflict": overlaps(guestSpans), "reason": ""})
            } else if (pending.indexOf(email) >= 0) {
                result.push({"label": guest, "state": "pending", "spans": [],
                             "conflict": false, "reason": ""})
            } else {
                result.push({"label": guest, "state": "unavailable", "spans": [],
                             "conflict": false, "reason": unavailableReason(email)})
            }
        }
        return result
    }

    function statusText(row) {
        if (row.state === "pending")
            return qsTr("Checking…")
        if (row.state === "unavailable")
            return row.reason === "unsupported" ? qsTr("Can't check")
                    : row.reason === "notFound" ? qsTr("Not shared")
                    : row.reason === "permission" ? qsTr("Needs permission")
                    : qsTr("Unavailable")
        return row.conflict ? qsTr("Busy") : qsTr("Free")
    }

    Repeater {
        model: root.rows
        delegate: RowLayout {
            id: personRow
            required property var modelData
            required property int index
            objectName: "availabilityRow-" + index
            readonly property string status: root.statusText(modelData)
            Layout.fillWidth: true
            spacing: Theme.spacingSM
            Accessible.role: Accessible.StaticText
            Accessible.name: modelData.label + ", " + status

            Text {
                Layout.preferredWidth: 130
                textFormat: Text.PlainText
                text: personRow.modelData.label
                color: Theme.text
                elide: Text.ElideRight
                font.pixelSize: Theme.smallFontSize
            }
            Rectangle {
                id: bar
                Layout.fillWidth: true
                Layout.preferredHeight: 14
                radius: 3
                color: Theme.surfaceAlt
                border.color: Theme.border
                readonly property real windowFrom: root.workDayStart * 60
                readonly property real windowLength: Math.max(60, (root.workDayEnd
                                                                   - root.workDayStart) * 60)
                function xFor(minute) {
                    return Math.max(0, Math.min(width, (minute - windowFrom)
                                                       / windowLength * width))
                }
                Repeater {
                    model: personRow.modelData.spans
                    delegate: Rectangle {
                        required property var modelData
                        x: bar.xFor(modelData.from)
                        width: Math.max(0, bar.xFor(modelData.to) - x)
                        height: bar.height
                        radius: 3
                        color: Theme.alpha(Theme.danger, 0.55)
                    }
                }
                Rectangle {
                    readonly property real from: root.textMinutes(root.startText)
                    readonly property real to: root.textMinutes(root.endText)
                    visible: !isNaN(from) && !isNaN(to)
                    x: bar.xFor(from)
                    width: Math.max(2, bar.xFor(to > from ? to : 1440) - x)
                    height: bar.height
                    color: "transparent"
                    border.color: Theme.accent
                    border.width: 2
                    radius: 3
                }
            }
            Text {
                objectName: "availabilityStatus-" + personRow.index
                Layout.preferredWidth: 80
                textFormat: Text.PlainText
                text: personRow.status
                color: personRow.modelData.conflict ? Theme.danger
                       : personRow.modelData.state === "known" ? Theme.success
                                                               : Theme.mutedText
                font.pixelSize: Theme.microFontSize
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.rows.length > 0
        Text {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: qsTr("%1:00–%2:00").arg(root.workDayStart).arg(root.workDayEnd)
                  + (root.timeZone ? "  ·  " + root.timeZone : "")
            color: Theme.mutedText
            font.pixelSize: Theme.microFontSize
        }
        AppButton {
            objectName: "findFreeTime"
            compact: true
            enabled: !root.anyPending
            text: qsTr("Find next free time")
            toolTipText: root.anyPending
                         ? qsTr("Waiting to hear who is free")
                         : qsTr("Move the event to the next time everyone who could be checked is free")
            onClicked: root.slotRequested()
        }
    }
}
