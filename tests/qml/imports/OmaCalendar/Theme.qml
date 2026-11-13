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
    // Not "fallback", so the smoke tests exercise the derived palette.
    readonly property string sourceName: "test"
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

    // ---- Calendar palette ---------------------------------------------
    // Swatches offered for calendar colours. They follow the active Omarchy
    // theme: its accent, info, success, warning and danger colours first, then
    // hue rotations of those, topped up from the Tokyo Night set when the
    // theme is too monochrome to give ten distinct colours. With no theme
    // file (sourceName "fallback") the Tokyo Night set is used as is. Entries
    // are "#rrggbb" strings because calendar colours are stored as hex.
    readonly property var fallbackCalendarPalette: [
        "#7aa2f7", "#bb9af7", "#f7768e", "#ff9e64", "#e0af68",
        "#9ece6a", "#73daca", "#2ac3de", "#7dcfff", "#c0caf5"
    ]
    readonly property var calendarPalette: calendarPaletteFrom(
        sourceName, [accent, info, success, warning, danger])

    function hexColor(colorValue) {
        const value = Qt.color(colorValue)
        function part(channel) {
            const digits = Math.round(Math.max(0, Math.min(1, channel)) * 255).toString(16)
            return digits.length < 2 ? "0" + digits : digits
        }
        return "#" + part(value.r) + part(value.g) + part(value.b)
    }

    function calendarPaletteFrom(themeSourceName, baseColors) {
        const size = fallbackCalendarPalette.length
        if (themeSourceName === "fallback" || !baseColors || baseColors.length === 0)
            return fallbackCalendarPalette.slice()

        const result = []
        function distance(first, second) {
            const a = Qt.color(first)
            const b = Qt.color(second)
            return Math.sqrt(Math.pow(a.r - b.r, 2) + Math.pow(a.g - b.g, 2)
                             + Math.pow(a.b - b.b, 2))
        }
        function add(colorValue) {
            if (result.length >= size)
                return
            const hex = hexColor(colorValue)
            for (let index = 0; index < result.length; ++index) {
                if (distance(result[index], hex) < 0.12)
                    return
            }
            result.push(hex)
        }

        // Greys make poor calendar colours and cannot be hue-rotated.
        const seeds = []
        for (let index = 0; index < baseColors.length; ++index) {
            const value = Qt.color(baseColors[index])
            if (value.hslHue >= 0 && value.hslSaturation >= 0.15)
                seeds.push(value)
        }
        for (let index = 0; index < seeds.length; ++index)
            add(seeds[index])

        const rotations = [0.5, 0.25, 0.75, 0.125, 0.375, 0.625, 0.875]
        for (let step = 0; step < rotations.length && result.length < size; ++step) {
            for (let index = 0; index < seeds.length && result.length < size; ++index) {
                const seed = seeds[index]
                const rotated = Qt.hsla((seed.hslHue + rotations[step]) % 1,
                                        Math.max(seed.hslSaturation, 0.45),
                                        Math.min(Math.max(seed.hslLightness, 0.45), 0.8), 1)
                // Keep derived swatches visible on the theme background.
                if (contrastRatio(rotated, background) >= 3)
                    add(rotated)
            }
        }
        for (let index = 0; index < size && result.length < size; ++index)
            add(fallbackCalendarPalette[index])
        // Near-duplicates of the fallback set can leave gaps; fill them
        // unconditionally so the palette always has the same size.
        for (let index = 0; result.length < size; ++index)
            result.push(fallbackCalendarPalette[index % size])
        return result
    }

    // A short human name for a colour, for screen readers: "Blue", "Teal",
    // "Light Blue". The buckets give the ten fallback swatches distinct names.
    function colorName(colorValue) {
        const value = Qt.color(colorValue)
        const lightness = value.hslLightness
        if (value.hslHue < 0 || value.hslSaturation < 0.12) {
            if (lightness < 0.15)
                return qsTr("Black")
            if (lightness > 0.9)
                return qsTr("White")
            return qsTr("Gray")
        }
        const hue = value.hslHue * 360
        let name = qsTr("Pink")
        if (hue < 12 || hue >= 345)
            name = qsTr("Red")
        else if (hue < 33)
            name = qsTr("Orange")
        else if (hue < 65)
            name = qsTr("Yellow")
        else if (hue < 80)
            name = qsTr("Lime")
        else if (hue < 150)
            name = qsTr("Green")
        else if (hue < 180)
            name = qsTr("Teal")
        else if (hue < 195)
            name = qsTr("Cyan")
        else if (hue < 210)
            name = qsTr("Sky blue")
        else if (hue < 255)
            name = qsTr("Blue")
        else if (hue < 315)
            name = qsTr("Purple")
        if (lightness > 0.8)
            return qsTr("Light %1").arg(name)
        if (lightness < 0.3)
            return qsTr("Dark %1").arg(name)
        return name
    }

    // ---- Contrast -----------------------------------------------------
    // WCAG 2 relative luminance and contrast ratio.

    function luminance(colorValue) {
        function channel(value) {
            return value <= 0.03928 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * channel(colorValue.r) + 0.7152 * channel(colorValue.g)
                + 0.0722 * channel(colorValue.b)
    }

    function contrastRatio(first, second) {
        const a = luminance(first)
        const b = luminance(second)
        return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05)
    }

    // The opaque colour of topColor drawn at opacity over bottomColor.
    function blend(topColor, opacity, bottomColor) {
        return Qt.rgba(topColor.r * opacity + bottomColor.r * (1 - opacity),
                       topColor.g * opacity + bottomColor.g * (1 - opacity),
                       topColor.b * opacity + bottomColor.b * (1 - opacity), 1)
    }

    // preferredColor when it reads at 4.5:1 on backgroundColor, otherwise the
    // theme's text or background colour, whichever contrasts more, and black
    // or white when neither does.
    function readableText(backgroundColor, preferredColor) {
        if (contrastRatio(preferredColor, backgroundColor) >= 4.5)
            return preferredColor
        const candidates = [text, background, Qt.rgba(0, 0, 0, 1), Qt.rgba(1, 1, 1, 1)]
        let best = candidates[0]
        for (let index = 1; index < candidates.length; ++index) {
            if (contrastRatio(candidates[index], backgroundColor)
                    > contrastRatio(best, backgroundColor))
                best = candidates[index]
        }
        return best
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
