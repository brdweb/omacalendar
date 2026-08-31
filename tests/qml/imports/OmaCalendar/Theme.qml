pragma Singleton
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
    readonly property int fontSize: 13
    readonly property int smallFontSize: 11
    readonly property int microFontSize: 10
    readonly property int titleFontSize: 23
    readonly property int displayFontSize: 31
    readonly property int radius: 12
    readonly property int smallRadius: 8
    readonly property int spacing: 12
    readonly property int controlHeight: 38
    readonly property int sidebarWidth: 272
    readonly property int panelWidth: 390

    function alpha(colorValue, opacity) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, opacity)
    }
}
