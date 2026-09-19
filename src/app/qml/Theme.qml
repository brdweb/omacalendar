pragma Singleton
// OmarchyTheme is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick

QtObject {
    readonly property color background: OmarchyTheme.background
    readonly property color darkBackground: OmarchyTheme.darkBackground
    readonly property color surface: OmarchyTheme.surface
    readonly property color surfaceAlt: OmarchyTheme.surfaceAlt
    readonly property color text: OmarchyTheme.text
    readonly property color mutedText: OmarchyTheme.mutedText
    readonly property color accent: OmarchyTheme.accent
    readonly property color accentText: OmarchyTheme.onAccent
    readonly property color danger: OmarchyTheme.danger
    readonly property color success: OmarchyTheme.success
    readonly property color warning: OmarchyTheme.warning
    readonly property color info: OmarchyTheme.info
    readonly property color border: alpha(text, 0.11)
    readonly property color divider: alpha(text, 0.075)
    readonly property color focus: alpha(accent, 0.72)
    readonly property color accentSoft: alpha(accent, 0.16)
    readonly property int fontSize: OmarchyTheme.baseFontSize
    readonly property int smallFontSize: Math.max(11, fontSize - 2)
    readonly property int microFontSize: Math.max(10, fontSize - 3)
    readonly property int titleFontSize: fontSize + 7
    readonly property int displayFontSize: fontSize + 14
    readonly property int iconFontSize: fontSize + 2

    // Spacing scale. Every margin and layout gap resolves to one of these.
    readonly property int spacingXS: 4
    readonly property int spacingSM: 8
    readonly property int spacingMD: 12
    readonly property int spacingLG: 16
    readonly property int spacingXL: 24

    // Three-step radius scale: SM for chips and swatches, MD for controls and
    // rows, LG for cards, dialogs and popups.
    readonly property int radiusSM: 6
    readonly property int radiusMD: 10
    readonly property int radiusLG: 14

    readonly property int controlHeight: 40
    readonly property int sidebarWidth: 272
    readonly property int panelWidth: 390
    readonly property int sidebarCollapseWidth: 1000

    // Single glyph set for icon-only controls.
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

    // ---- Date and time formatting -------------------------------------
    // timeFormat is the stored preference: "system", "12h" or "24h".

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
