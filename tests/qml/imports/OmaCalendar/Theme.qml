pragma Singleton
// Test shim mirroring src/app/qml/Theme.qml with static values: the smoke
// tests run without the OmarchyTheme context property or the theme bridge.
// Keep every property and function of the real theme available with the same
// name and type so the app QML under test resolves identically.
import QtQuick

QtObject {
    readonly property color background: "#1a1b26"
    readonly property color darkBackground: "#16161e"
    readonly property color surface: "#24283b"
    readonly property color surfaceAlt: "#2f3549"
    readonly property color text: "#c0caf5"
    readonly property color mutedText: "#8b93b6"
    readonly property color accent: "#7aa2f7"
    readonly property color accentText: "#16161e"
    readonly property color danger: "#f7768e"
    readonly property color success: "#9ece6a"
    readonly property color warning: "#e0af68"
    readonly property color info: "#7dcfff"
    readonly property color border: alpha(text, 0.11)
    readonly property color divider: alpha(text, 0.075)
    readonly property color focus: alpha(accent, 0.72)
    readonly property color accentSoft: alpha(accent, 0.16)
    readonly property int baseFontSize: 13
    readonly property int fontSize: baseFontSize
    readonly property int smallFontSize: Math.max(11, fontSize - 2)
    readonly property int microFontSize: Math.max(10, fontSize - 3)
    readonly property int titleFontSize: fontSize + 7
    readonly property int displayFontSize: fontSize + 14
    readonly property int iconFontSize: fontSize + 2

    readonly property int spacingXS: 4
    readonly property int spacingSM: 8
    readonly property int spacingMD: 12
    readonly property int spacingLG: 16
    readonly property int spacingXL: 24

    readonly property int radiusSM: 6
    readonly property int radiusMD: 10
    readonly property int radiusLG: 14

    // Retired tokens kept for compatibility with older checkouts of the app
    // QML; new code should use the scale tokens above.
    readonly property int radius: radiusMD
    readonly property int smallRadius: radiusSM
    readonly property int spacing: spacingMD

    readonly property int controlHeight: 40
    readonly property int sidebarWidth: 272
    readonly property int panelWidth: 390
    readonly property int sidebarCollapseWidth: 1000

    readonly property string glyphPrevious: "‹"
    readonly property string glyphNext: "›"
    readonly property string glyphUp: "↑"
    readonly property string glyphDown: "↓"
    readonly property string glyphAdd: "+"
    readonly property string glyphRemove: "×"
    readonly property string glyphSearch: "⌕"
    readonly property string glyphSidebarShown: "◧"
    readonly property string glyphSidebarHidden: "◨"
    readonly property string glyphSettings: "⚙"
    readonly property string glyphInvitation: "◇"
    readonly property string glyphCalendar: "◫"
    readonly property string glyphDragHandle: "≡"

    function alpha(colorValue, opacity) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, opacity)
    }

    function timePattern(timeFormat) {
        if (timeFormat === "24h")
            return "HH:mm"
        if (timeFormat === "12h")
            return "h:mm AP"
        return Qt.locale().timeFormat(Locale.ShortFormat)
    }

    function hourPattern(timeFormat) {
        if (timeFormat === "24h")
            return "HH:mm"
        if (timeFormat === "12h")
            return "h AP"
        return Qt.locale().timeFormat(Locale.ShortFormat)
    }

    function formatTime(value, timeFormat) {
        if (!value || isNaN(value.getTime()))
            return ""
        return Qt.formatTime(value, timePattern(timeFormat))
    }

    function formatHour(value, timeFormat) {
        if (!value || isNaN(value.getTime()))
            return ""
        return Qt.formatTime(value, hourPattern(timeFormat))
    }

    function monthDay(value) {
        if (!value || isNaN(value.getTime()))
            return ""
        return Qt.formatDate(value, "MMMM d")
    }

    function shortDate(value) {
        if (!value || isNaN(value.getTime()))
            return ""
        return Qt.formatDate(value, "ddd, MMM d")
    }

    function sameDate(first, second) {
        return Boolean(first) && Boolean(second)
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }

    function dayOffset(value, reference) {
        const left = new Date(value.getFullYear(), value.getMonth(), value.getDate())
        const right = new Date(reference.getFullYear(), reference.getMonth(),
                               reference.getDate())
        return Math.round((left.getTime() - right.getTime()) / 86400000)
    }

    function relativeDayLabel(value, referenceValue) {
        if (!value || isNaN(value.getTime()))
            return ""
        const reference = referenceValue ? referenceValue : new Date()
        const offset = dayOffset(value, reference)
        if (offset === 0)
            return qsTr("Today")
        if (offset === 1)
            return qsTr("Tomorrow")
        if (offset === -1)
            return qsTr("Yesterday")
        return Qt.formatDate(value, "dddd")
    }

    function dateTimeLabel(value, timeFormat) {
        if (!value || isNaN(value.getTime()))
            return ""
        return shortDate(value) + " · " + formatTime(value, timeFormat)
    }

    function timeRange(startValue, endValue, allDay, timeFormat) {
        if (allDay === true)
            return qsTr("All day")
        const startText = formatTime(startValue, timeFormat)
        const endText = formatTime(endValue, timeFormat)
        if (startText.length === 0)
            return endText
        if (endText.length === 0)
            return startText
        return startText + " – " + endText
    }

    function relativeSince(value, referenceValue) {
        if (!value || isNaN(value.getTime()))
            return ""
        const reference = referenceValue ? referenceValue : new Date()
        const seconds = Math.round((reference.getTime() - value.getTime()) / 1000)
        if (seconds < 0)
            return qsTr("just now")
        if (seconds < 60)
            return qsTr("just now")
        const minutes = Math.floor(seconds / 60)
        if (minutes < 60)
            return minutes + qsTr(" min ago")
        const hours = Math.floor(minutes / 60)
        if (hours < 24)
            return hours + qsTr(" h ago")
        const days = Math.floor(hours / 24)
        if (days < 7)
            return days + qsTr(" d ago")
        return shortDate(value)
    }
}
