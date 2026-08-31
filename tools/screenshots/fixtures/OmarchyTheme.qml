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
    // QML reserves names beginning with "on" for signal handlers. The capture
    // script maps the production bridge's C++ `onAccent` property to this
    // fixture-only equivalent in its temporary copy of Theme.qml.
    readonly property color accentForeground: "#16161e"
    readonly property color danger: "#f7768e"
    readonly property color success: "#9ece6a"
    readonly property color warning: "#e0af68"
    readonly property color info: "#7dcfff"
    readonly property int baseFontSize: 13
    readonly property string sourceName: "Tokyo Night"
}
