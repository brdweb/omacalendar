import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

ItemDelegate {
    id: root
    required property var eventData
    readonly property bool generatedInstance: Boolean(eventData.recurrenceRule
                                                       && eventData.recurrenceId)
    signal editRequested(var eventData)

    width: ListView.view ? ListView.view.width : 400
    height: Math.max(68, details.implicitHeight + 24)
    padding: 0
    onClicked: {
        if (!generatedInstance) editRequested(eventData)
    }
    ToolTip.visible: generatedInstance && hovered
    ToolTip.text: "Recurring instance editing will be enabled after the credential checkpoint"
    ToolTip.delay: 450

    background: Rectangle {
        radius: Theme.radius
        color: root.hovered ? Theme.surfaceAlt : Theme.surface
        border.width: 1
        border.color: Theme.alpha(Theme.text, root.hovered ? 0.18 : 0.08)
    }

    contentItem: RowLayout {
        spacing: 14
        Rectangle {
            Layout.preferredWidth: 4
            Layout.fillHeight: true
            Layout.topMargin: 10
            Layout.bottomMargin: 10
            radius: 2
            color: root.eventData.calendarColor || Theme.accent
        }
        ColumnLayout {
            id: details
            Layout.fillWidth: true
            Layout.leftMargin: 2
            Layout.rightMargin: 12
            spacing: 3
            Text {
                Layout.fillWidth: true
                text: root.eventData.summary || "Untitled event"
                color: Theme.text
                font.pixelSize: Theme.fontSize
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: {
                    if (root.eventData.allDay) return "All day"
                    const start = new Date(root.eventData.startUtc)
                    const end = new Date(root.eventData.endUtc)
                    return Qt.formatTime(start, "h:mm AP") + " – " + Qt.formatTime(end, "h:mm AP")
                           + (root.eventData.location ? "  ·  " + root.eventData.location : "")
                           + (root.generatedInstance ? "  ·  Recurring" : "")
                }
                color: Theme.mutedText
                font.pixelSize: Theme.smallFontSize
                elide: Text.ElideRight
            }
        }
        Rectangle {
            visible: root.eventData.dirty === true
            Layout.preferredWidth: 8
            Layout.preferredHeight: 8
            Layout.rightMargin: 14
            radius: 4
            color: Theme.accent
            Accessible.name: "Pending synchronization"
        }
    }
}
