pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import OmaCalendar

Rectangle {
    id: root

    property string text: ""
    property string tone: "neutral"
    property bool dotOnly: false
    readonly property color toneColor: {
        if (tone === "success")
            return Theme.success
        if (tone === "warning")
            return Theme.warning
        if (tone === "danger")
            return Theme.danger
        if (tone === "info")
            return Theme.info
        return Theme.mutedText
    }

    implicitWidth: dotOnly ? 10 : badgeRow.implicitWidth + 16
    implicitHeight: dotOnly ? 10 : 24
    radius: height / 2
    color: dotOnly ? toneColor : Theme.alpha(toneColor, 0.14)
    border.color: dotOnly ? "transparent" : Theme.alpha(toneColor, 0.26)
    Accessible.name: text

    RowLayout {
        id: badgeRow
        visible: !root.dotOnly
        anchors.centerIn: parent
        spacing: 6

        Rectangle {
            Layout.preferredWidth: 6
            Layout.preferredHeight: 6
            radius: 3
            color: root.toneColor
        }
        Text {
            text: root.text
            color: Theme.text
            font.pixelSize: Theme.microFontSize
            font.weight: Font.DemiBold
        }
    }
}
