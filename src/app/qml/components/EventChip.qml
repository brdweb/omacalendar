pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

ItemDelegate {
    id: root

    required property var eventData
    property bool compact: false
    property bool showTime: true
    property bool selected: false
    property bool draggable: false
    property bool dragging: false
    property string timeText: ""
    property real dragOriginX: 0
    property real dragOriginY: 0
    signal activated(var eventData)
    signal dragStarted(var eventData)
    signal dragFinished(var eventData)

    readonly property color eventColor: eventData.calendarColor
                                        || eventData.color || Theme.accent
    readonly property string stateText: {
        if (eventData.conflict === true || eventData.operationState === "blocked")
            return "Conflict"
        if (eventData.operationState === "retry_wait")
            return "Retrying"
        if (eventData.operationState === "failed"
                || eventData.operationState === "error")
            return "Failed"
        if (eventData.dirty === true || eventData.operationState === "pending")
            return "Pending"
        if (eventData.readOnly === true)
            return "Read only"
        return ""
    }

    implicitHeight: compact ? 24 : 36
    padding: 0
    hoverEnabled: true
    opacity: dragging ? 0.82 : 1
    z: dragging ? 100 : 0
    Accessible.name: (eventData.summary || "Untitled event")
                     + (timeText.length > 0 ? ", " + timeText : "")
    Accessible.description: stateText
    Accessible.role: Accessible.Button
    onClicked: activated(eventData)

    Drag.active: dragHandler.active
    Drag.source: root
    Drag.keys: ["omacalendar-event"]
    Drag.hotSpot.x: width / 2
    Drag.hotSpot.y: height / 2

    DragHandler {
        id: dragHandler
        enabled: root.draggable
        target: root
        xAxis.enabled: true
        yAxis.enabled: true
        onActiveChanged: {
            if (active) {
                root.dragOriginX = root.x
                root.dragOriginY = root.y
                root.dragging = true
                root.dragStarted(root.eventData)
            } else if (root.dragging) {
                root.Drag.drop()
                root.x = root.dragOriginX
                root.y = root.dragOriginY
                root.dragging = false
                root.dragFinished(root.eventData)
            }
        }
    }

    background: Rectangle {
        radius: root.compact ? 5 : Theme.smallRadius
        color: root.selected ? Theme.alpha(root.eventColor, 0.28)
                             : root.hovered || root.activeFocus
                               ? Theme.alpha(root.eventColor, 0.22)
                               : Theme.alpha(root.eventColor, 0.13)
        border.width: root.activeFocus || root.selected ? 1 : 0
        border.color: root.activeFocus ? Theme.focus
                                       : Theme.alpha(root.eventColor, 0.48)
    }

    contentItem: RowLayout {
        spacing: root.compact ? 5 : 8

        Rectangle {
            Layout.preferredWidth: root.compact ? 3 : 4
            Layout.fillHeight: true
            Layout.topMargin: root.compact ? 4 : 6
            Layout.bottomMargin: root.compact ? 4 : 6
            radius: 2
            color: root.eventColor
        }
        Text {
            visible: root.showTime && root.timeText.length > 0
            text: root.timeText
            color: Theme.mutedText
            font.pixelSize: root.compact ? Theme.microFontSize : Theme.smallFontSize
        }
        Text {
            Layout.fillWidth: true
            text: root.eventData.summary || "Untitled event"
            color: Theme.text
            font.pixelSize: root.compact ? Theme.microFontSize : Theme.smallFontSize
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Rectangle {
            visible: root.stateText.length > 0 && root.compact
            Layout.preferredWidth: 7
            Layout.preferredHeight: 7
            radius: 4
            color: root.stateText === "Conflict" || root.stateText === "Failed"
                  ? Theme.danger
                  : root.stateText === "Retrying" ? Theme.warning
                  : root.stateText === "Pending" ? Theme.info
                                                 : Theme.mutedText
        }
        Text {
            visible: root.stateText.length > 0 && !root.compact
            text: root.stateText
            color: root.stateText === "Conflict" ? Theme.danger : Theme.mutedText
            font.pixelSize: Theme.microFontSize
        }
    }
}
