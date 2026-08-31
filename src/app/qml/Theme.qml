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
    readonly property int radius: 12
    readonly property int smallRadius: 8
    readonly property int spacing: 12
    readonly property int controlHeight: 40
    readonly property int sidebarWidth: 272
    readonly property int panelWidth: 390

    function alpha(colorValue, opacity) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, opacity)
    }
}
