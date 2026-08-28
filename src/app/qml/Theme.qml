pragma Singleton
import QtQuick

QtObject {
    readonly property color background: OmarchyTheme.background
    readonly property color darkBackground: OmarchyTheme.darkBackground
    readonly property color surface: OmarchyTheme.surface
    readonly property color surfaceAlt: OmarchyTheme.surfaceAlt
    readonly property color text: OmarchyTheme.text
    readonly property color mutedText: OmarchyTheme.mutedText
    readonly property color accent: OmarchyTheme.accent
    readonly property color danger: OmarchyTheme.danger
    readonly property color success: OmarchyTheme.success
    readonly property int fontSize: OmarchyTheme.baseFontSize
    readonly property int smallFontSize: Math.max(11, fontSize - 2)
    readonly property int titleFontSize: fontSize + 10
    readonly property int radius: 12
    readonly property int spacing: 12

    function alpha(colorValue, opacity) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, opacity)
    }
}

