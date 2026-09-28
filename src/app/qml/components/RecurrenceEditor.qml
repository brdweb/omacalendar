pragma ComponentBehavior: Bound
// App is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick
import QtQuick.Layouts
import OmaCalendar
import "../RecurrenceRule.js" as RecurrenceRule

// Structured repeat controls for the event editor. Rules outside the subset
// these controls express load as "Custom rule" text and are saved unchanged.
ColumnLayout {
    id: root

    property date startDate: new Date()
    property bool allDayEvent: false
    property bool floating: false
    property string timeZone: ""
    property bool editable: true

    // 0: does not repeat, 1-4: daily/weekly/monthly/yearly, 5: custom text.
    property int mode: 0
    property int interval: 1
    property var weekdays: []
    property string monthlyMode: "monthday"
    property string endMode: "never"
    property int count: 10

    readonly property var frequencies: ["", "DAILY", "WEEKLY", "MONTHLY", "YEARLY"]
    readonly property var weekdayCodes: ["MO", "TU", "WE", "TH", "FR", "SA", "SU"]
    readonly property var monthPosition: RecurrenceRule.weekdayOrdinal(startDate)
    readonly property string preview: mode >= 1 && mode <= 4
                                      ? RecurrenceRule.describe(currentSpec(), startDate,
                                                                untilField.text)
                                      : ""

    spacing: Theme.spacingSM

    function load(ruleText) {
        const text = String(ruleText || "").trim()
        customField.text = ""
        interval = 1
        weekdays = [RecurrenceRule.weekdayCode(startDate)]
        monthlyMode = "monthday"
        endMode = "never"
        count = 10
        untilField.text = ""
        if (!text) {
            mode = 0
            return
        }
        const spec = RecurrenceRule.parse(text, startDate)
        if (!spec.representable) {
            mode = 5
            customField.text = text.replace(/^RRULE:/i, "")
            return
        }
        mode = frequencies.indexOf(spec.frequency)
        interval = spec.interval
        weekdays = spec.weekdays
        monthlyMode = spec.monthlyMode
        endMode = spec.endMode
        count = spec.count
        untilField.text = spec.endMode === "until" ? untilDateText(spec.until) : ""
    }

    function currentSpec() {
        return {
            "representable": true,
            "frequency": frequencies[mode] || "WEEKLY",
            "interval": interval,
            "weekdays": weekdays,
            "monthlyMode": monthlyMode,
            "endMode": endMode,
            "until": endMode === "until" ? untilValue(untilField.text) : "",
            "count": count
        }
    }

    // The rule to save, or "" when the event does not repeat.
    function rule() {
        if (mode === 0)
            return ""
        if (mode === 5)
            return customField.text.trim().replace(/^RRULE:/i, "")
        return RecurrenceRule.build(currentSpec(), startDate)
    }

    function validationError() {
        if (mode === 5 && customField.text.trim().length === 0)
            return qsTr("Enter a recurrence rule for the custom repeat option.")
        if (mode === 2 && weekdays.length === 0)
            return qsTr("Choose at least one weekday.")
        if (mode >= 1 && mode <= 4 && endMode === "until") {
            const until = new Date(untilField.text + "T00:00:00")
            if (!/^\d{4}-\d{2}-\d{2}$/.test(untilField.text) || isNaN(until.getTime()))
                return qsTr("Enter the last repeat date as YYYY-MM-DD.")
            const startDay = new Date(startDate.getFullYear(), startDate.getMonth(),
                                      startDate.getDate())
            if (until < startDay)
                return qsTr("The last repeat date must be on or after the start.")
            if (untilValue(untilField.text) === "")
                return qsTr("That end date does not exist in the event's time zone.")
        }
        return ""
    }

    // UNTIL for the chosen last day: a date for all-day events, local end of day
    // for floating times, and that instant in UTC for zoned events.
    function untilValue(dateText) {
        if (!/^\d{4}-\d{2}-\d{2}$/.test(dateText))
            return ""
        const compact = dateText.replace(/-/g, "")
        if (allDayEvent)
            return compact
        if (floating)
            return compact + "T235959"
        const utc = App.wallTimeToUtc(dateText, "23:59", timeZone)
        if (!utc)
            return ""
        return utc.slice(0, 19).replace(/[-:]/g, "") + "Z"
    }

    // The last repeat day, as the user reads it, from an UNTIL value.
    function untilDateText(untilText) {
        const value = String(untilText || "")
        const dateText = value.slice(0, 4) + "-" + value.slice(4, 6) + "-" + value.slice(6, 8)
        if (value.length === 16 && value.endsWith("Z") && !allDayEvent && !floating) {
            const iso = dateText + "T" + value.slice(9, 11) + ":" + value.slice(11, 13)
                    + ":" + value.slice(13, 15) + "Z"
            const wall = App.utcToWallTime(iso, timeZone)
            if (wall)
                return wall.slice(0, 10)
        }
        return dateText
    }

    function toggleWeekday(code) {
        const next = weekdays.slice()
        const index = next.indexOf(code)
        if (index >= 0)
            next.splice(index, 1)
        else
            next.push(code)
        weekdays = weekdayCodes.filter(function(day) { return next.indexOf(day) >= 0 })
    }

    function unitName() {
        const plural = interval > 1
        return {
            1: plural ? qsTr("days") : qsTr("day"),
            2: plural ? qsTr("weeks") : qsTr("week"),
            3: plural ? qsTr("months") : qsTr("month"),
            4: plural ? qsTr("years") : qsTr("year")
        }[mode] || ""
    }

    AppComboBox {
        id: modeBox
        objectName: "recurrenceMode"
        Layout.fillWidth: true
        model: [qsTr("Does not repeat"), qsTr("Daily"), qsTr("Weekly"), qsTr("Monthly"),
                qsTr("Yearly"), qsTr("Custom rule")]
        currentIndex: root.mode
        enabled: root.editable
        Accessible.name: qsTr("Event recurrence")
        onActivated: index => {
            if (index === 5 && root.mode >= 1 && root.mode <= 4)
                customField.text = root.rule()
            root.mode = index
        }
    }

    RowLayout {
        visible: root.mode >= 1 && root.mode <= 4
        Layout.fillWidth: true
        spacing: Theme.spacingSM
        Text {
            textFormat: Text.PlainText
            text: qsTr("Every")
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
        }
        AppSpinBox {
            objectName: "recurrenceInterval"
            from: 1
            to: 99
            value: root.interval
            enabled: root.editable
            Accessible.name: qsTr("Repeat interval")
            onValueModified: root.interval = value
        }
        Text {
            textFormat: Text.PlainText
            text: root.unitName()
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
        }
        Item { Layout.fillWidth: true }
    }

    Flow {
        visible: root.mode === 2
        Layout.fillWidth: true
        spacing: Theme.spacingXS
        Repeater {
            model: root.weekdayCodes
            delegate: AppButton {
                required property string modelData
                required property int index
                objectName: "recurrenceWeekday-" + modelData
                compact: true
                primary: root.weekdays.indexOf(modelData) >= 0
                text: Qt.locale().dayName((index + 1) % 7, Locale.ShortFormat)
                enabled: root.editable
                Accessible.name: Qt.locale().dayName((index + 1) % 7, Locale.LongFormat)
                Accessible.checkable: true
                Accessible.checked: primary
                onClicked: root.toggleWeekday(modelData)
            }
        }
    }

    AppComboBox {
        id: monthlyBox
        objectName: "recurrenceMonthlyMode"
        visible: root.mode === 3
        Layout.fillWidth: true
        model: {
            const options = [
                {"text": qsTr("On day %1").arg(root.startDate.getDate()), "value": "monthday"},
                {"text": qsTr("On the %1 %2")
                             .arg(RecurrenceRule.ordinalText(root.monthPosition.ordinal))
                             .arg(RecurrenceRule.weekdayName(root.monthPosition.weekday)),
                 "value": "weekday"}
            ]
            if (root.monthPosition.isLast)
                options.push({"text": qsTr("On the last %1").arg(
                                          RecurrenceRule.weekdayName(root.monthPosition.weekday)),
                              "value": "lastWeekday"})
            return options
        }
        textRole: "text"
        valueRole: "value"
        currentIndex: Math.max(0, indexOfValue(root.monthlyMode))
        enabled: root.editable
        Accessible.name: qsTr("Monthly repeat day")
        onActivated: root.monthlyMode = currentValue
    }

    RowLayout {
        visible: root.mode >= 1 && root.mode <= 4
        Layout.fillWidth: true
        spacing: Theme.spacingSM
        AppComboBox {
            id: endBox
            objectName: "recurrenceEnd"
            Layout.fillWidth: true
            model: [{"text": qsTr("Never ends"), "value": "never"},
                    {"text": qsTr("Ends on date"), "value": "until"},
                    {"text": qsTr("Ends after"), "value": "count"}]
            textRole: "text"
            valueRole: "value"
            currentIndex: Math.max(0, indexOfValue(root.endMode))
            enabled: root.editable
            Accessible.name: qsTr("Repeat end")
            onActivated: {
                root.endMode = currentValue
                if (root.endMode === "until" && untilField.text.length === 0) {
                    const defaultEnd = new Date(root.startDate.getFullYear(),
                                                root.startDate.getMonth() + 3,
                                                root.startDate.getDate())
                    untilField.text = Qt.formatDate(defaultEnd, "yyyy-MM-dd")
                }
            }
        }
        AppTextField {
            id: untilField
            objectName: "recurrenceUntil"
            visible: root.endMode === "until"
            Layout.fillWidth: true
            placeholderText: qsTr("YYYY-MM-DD")
            accessibleName: qsTr("Last repeat date")
            enabled: root.editable
        }
        AppSpinBox {
            objectName: "recurrenceCount"
            visible: root.endMode === "count"
            from: 1
            to: 999
            value: root.count
            enabled: root.editable
            Accessible.name: qsTr("Number of occurrences")
            onValueModified: root.count = value
        }
        Text {
            textFormat: Text.PlainText
            visible: root.endMode === "count"
            text: qsTr("times")
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
        }
    }

    Text {
        objectName: "recurrencePreview"
        visible: root.preview.length > 0
        Layout.fillWidth: true
        textFormat: Text.PlainText
        text: root.preview
        color: Theme.mutedText
        wrapMode: Text.Wrap
        font.pixelSize: Theme.smallFontSize
    }

    AppTextField {
        id: customField
        objectName: "recurrenceCustomRule"
        visible: root.mode === 5
        Layout.fillWidth: true
        placeholderText: qsTr("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO")
        accessibleName: qsTr("Custom recurrence rule")
        enabled: root.editable
    }
}
