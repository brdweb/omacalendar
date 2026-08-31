pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

Control {
    id: root

    required property var eventData
    property int startMinute: 0
    property int durationMinutes: 60
    property int minimumDurationMinutes: 15
    property int maximumDurationMinutes: 527040
    property real pixelsPerHour: 64
    property real dayWidth: width
    property bool horizontalRescheduleEnabled: false
    property bool editable: eventData.readOnly !== true
                            && eventData.conflict !== true
                            && eventData.dirty !== true
                            && String(eventData.operationState
                                      || eventData.syncState || "") !== "pending"
                            && String(eventData.operationState
                                      || eventData.syncState || "") !== "sending"
                            && String(eventData.operationState
                                      || eventData.syncState || "") !== "blocked"
                            && String(eventData.operationState
                                      || eventData.syncState || "") !== "retry_wait"
                            && String(eventData.operationState
                                      || eventData.syncState || "") !== "failed"
                            && String(eventData.operationState
                                      || eventData.syncState || "") !== "error"
    property bool selected: false
    property real moveDeltaX: 0
    property real moveDelta: 0
    property real startResizeDelta: 0
    property real resizeDelta: 0
    property real pressX: 0
    property real pressY: 0
    signal activated(var eventData)
    signal rescheduleRequested(var eventData, int startMinute,
                               int durationMinutes, int dayOffset)

    readonly property color eventColor: eventData.calendarColor
                                        || eventData.color || Theme.accent
    readonly property string operationState: String(eventData.operationState
                                                    || eventData.syncState || "")
    readonly property string stateText: {
        if (eventData.conflict === true || operationState === "blocked")
            return "Conflict"
        if (operationState === "retry_wait")
            return "Retrying"
        if (operationState === "failed" || operationState === "error")
            return "Failed"
        if (eventData.dirty === true || operationState === "pending"
                || operationState === "sending")
            return "Pending"
        if (eventData.readOnly === true)
            return "Read only"
        return ""
    }
    readonly property bool interacting: moveArea.pressed
                                        || startResizeArea.pressed
                                        || endResizeArea.pressed
    readonly property bool resizable: editable
                                      && durationMinutes >= minimumDurationMinutes
    readonly property int snappedMoveMinutes: Math.round(moveDelta / pixelsPerHour
                                                         * 4) * 15
    readonly property int snappedStartResizeMinutes:
        Math.round(startResizeDelta / pixelsPerHour * 4) * 15
    readonly property int snappedResizeMinutes: Math.round(resizeDelta / pixelsPerHour
                                                           * 4) * 15
    readonly property int snappedDayOffset:
        horizontalRescheduleEnabled && dayWidth > 0
        ? Math.round(moveDeltaX / dayWidth) : 0
    readonly property real visualX: horizontalRescheduleEnabled ? moveDeltaX : 0
    readonly property real visualY: moveDelta + startResizeDelta
    readonly property real visualHeight: Math.max(24, height - startResizeDelta
                                                  + resizeDelta)

    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.name: eventData.summary || "Untitled event"
    Accessible.description: stateText
    Accessible.role: Accessible.Button
    Keys.onReturnPressed: root.activated(root.eventData)
    Keys.onEnterPressed: root.activated(root.eventData)

    background: Item {
        Rectangle {
            x: root.visualX
            y: root.visualY
            width: parent.width
            height: root.visualHeight
            radius: 7
            color: Theme.alpha(root.eventColor,
                               root.selected ? 0.34 : root.hovered ? 0.28 : 0.21)
            border.width: root.activeFocus || root.selected ? 2 : 1
            border.color: root.activeFocus || root.selected ? Theme.focus
                                           : Theme.alpha(root.eventColor, 0.62)
            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 4
                radius: 2
                color: root.eventColor
            }
            Rectangle {
                visible: root.stateText.length > 0
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 5
                width: 7
                height: 7
                radius: 4
                color: root.stateText === "Conflict" || root.stateText === "Failed"
                      ? Theme.danger
                      : root.stateText === "Retrying" ? Theme.warning
                      : root.stateText === "Pending" ? Theme.info
                                                     : Theme.mutedText
            }
        }
    }

    contentItem: Item {
        Item {
            x: root.visualX
            y: root.visualY
            width: parent.width
            height: root.visualHeight
            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 7
                anchors.topMargin: 5
                anchors.bottomMargin: 5
                spacing: 1

                Text {
                    Layout.fillWidth: true
                    text: root.eventData.summary || "Untitled event"
                    color: Theme.text
                    font.pixelSize: Theme.smallFontSize
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    visible: root.visualHeight >= 43
                    Layout.fillWidth: true
                    text: root.eventData.location || ""
                    color: Theme.mutedText
                    font.pixelSize: Theme.microFontSize
                    elide: Text.ElideRight
                }
                Text {
                    visible: root.stateText.length > 0 && root.visualHeight >= 58
                    Layout.fillWidth: true
                    text: root.stateText
                    color: root.stateText === "Conflict" || root.stateText === "Failed"
                          ? Theme.danger
                          : Theme.mutedText
                    font.pixelSize: Theme.microFontSize
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
            }
        }
    }

    MouseArea {
        id: moveArea
        anchors.fill: parent
        anchors.bottomMargin: 8
        cursorShape: root.editable ? Qt.OpenHandCursor : Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton
        preventStealing: root.editable
        onPressed: mouse => {
            root.pressX = mouse.x
            root.pressY = mouse.y
            cursorShape = root.editable ? Qt.ClosedHandCursor : Qt.PointingHandCursor
        }
        onPositionChanged: mouse => {
            if (pressed && root.editable) {
                root.moveDeltaX = mouse.x - root.pressX
                root.moveDelta = mouse.y - root.pressY
            }
        }
        onReleased: {
            cursorShape = root.editable ? Qt.OpenHandCursor : Qt.PointingHandCursor
            root.commitMove(root.moveDeltaX, root.moveDelta)
        }
        onCanceled: {
            root.moveDeltaX = 0
            root.moveDelta = 0
        }
    }

    MouseArea {
        id: startResizeArea
        objectName: "timelineStartResizeHandle"
        visible: root.resizable
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 10
        z: 2
        hoverEnabled: true
        cursorShape: Qt.SizeVerCursor
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        onPressed: mouse => root.pressY = mouse.y
        onPositionChanged: mouse => {
            if (pressed)
                root.startResizeDelta = mouse.y - root.pressY
        }
        onReleased: root.commitStartResize(root.startResizeDelta)
        onCanceled: root.startResizeDelta = 0
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 2
            width: Math.min(34, parent.width - 16)
            height: 2
            radius: 1
            visible: startResizeArea.containsMouse || startResizeArea.pressed
            color: Theme.alpha(Theme.text, 0.72)
        }
    }

    MouseArea {
        id: endResizeArea
        objectName: "timelineEndResizeHandle"
        visible: root.resizable
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 10
        z: 2
        hoverEnabled: true
        cursorShape: Qt.SizeVerCursor
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        onPressed: mouse => root.pressY = mouse.y
        onPositionChanged: mouse => {
            if (pressed)
                root.resizeDelta = mouse.y - root.pressY
        }
        onReleased: root.commitEndResize(root.resizeDelta)
        onCanceled: root.resizeDelta = 0
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            width: Math.min(34, parent.width - 16)
            height: 2
            radius: 1
            visible: endResizeArea.containsMouse || endResizeArea.pressed
            color: Theme.alpha(Theme.text, 0.72)
        }
    }

    function commitMove(horizontalDelta, verticalDelta) {
        moveDeltaX = horizontalDelta
        moveDelta = verticalDelta
        const click = Math.abs(horizontalDelta) < 4 && Math.abs(verticalDelta) < 4
        const absoluteStart = startMinute + snappedMoveMinutes
        const dayOffset = snappedDayOffset + Math.floor(absoluteStart / 1440)
        const nextStart = normalizedMinute(absoluteStart)
        moveDeltaX = 0
        moveDelta = 0
        if (click || !editable) {
            activated(eventData)
            return
        }
        if (nextStart === startMinute && dayOffset === 0)
            return
        rescheduleRequested(eventData, nextStart, durationMinutes, dayOffset)
    }

    function commitStartResize(delta) {
        startResizeDelta = delta
        const requestedDelta = snappedStartResizeMinutes
        const appliedDelta = Math.max(durationMinutes - maximumDurationMinutes,
                                      Math.min(durationMinutes
                                               - minimumDurationMinutes,
                                               requestedDelta))
        const absoluteStart = startMinute + appliedDelta
        const nextStart = normalizedMinute(absoluteStart)
        const dayOffset = Math.floor(absoluteStart / 1440)
        startResizeDelta = 0
        if (!resizable || appliedDelta === 0)
            return
        rescheduleRequested(eventData, nextStart,
                            durationMinutes - appliedDelta, dayOffset)
    }

    function commitEndResize(delta) {
        resizeDelta = delta
        const nextDuration = Math.max(minimumDurationMinutes,
                                      Math.min(maximumDurationMinutes,
                                               durationMinutes
                                               + snappedResizeMinutes))
        resizeDelta = 0
        if (!resizable || nextDuration === durationMinutes)
            return
        rescheduleRequested(eventData, startMinute, nextDuration, 0)
    }

    function normalizedMinute(value) {
        return ((value % 1440) + 1440) % 1440
    }
}
