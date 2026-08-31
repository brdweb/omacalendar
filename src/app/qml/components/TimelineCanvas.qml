pragma ComponentBehavior: Bound
import QtQuick
import OmaCalendar

Item {
    id: root

    property int firstHour: 0
    property int lastHour: 24
    property real pixelsPerHour: 64
    property int defaultDurationMinutes: 60
    property bool creationEnabled: true
    property int anchorMinute: 0
    property int currentMinute: 0
    property real pressPositionY: 0
    property bool moved: false
    signal createRequested(int startMinute, int durationMinutes)

    readonly property int selectionStartMinute: Math.min(anchorMinute, currentMinute)
    readonly property int selectionEndMinute: Math.max(anchorMinute, currentMinute)
    readonly property int selectionDurationMinutes:
        Math.max(15, selectionEndMinute - selectionStartMinute)

    MouseArea {
        id: creationArea
        objectName: "timelineCreationArea"
        anchors.fill: parent
        enabled: root.creationEnabled
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.CrossCursor
        preventStealing: true

        onPressed: mouse => {
            root.anchorMinute = root.minuteAt(mouse.y)
            root.currentMinute = root.anchorMinute
            root.pressPositionY = mouse.y
            root.moved = false
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            root.currentMinute = root.minuteAt(mouse.y)
            if (Math.abs(mouse.y - root.pressPositionY) >= 4)
                root.moved = true
        }
        onReleased: mouse => {
            root.currentMinute = root.minuteAt(mouse.y)
            const startMinute = root.moved
                    ? root.selectionStartMinute : root.anchorMinute
            const requestedDuration = root.moved
                    ? root.selectionDurationMinutes
                    : Math.max(15, root.defaultDurationMinutes)
            const durationMinutes = Math.max(15, Math.min(requestedDuration,
                                                           1440 - startMinute))
            root.createRequested(startMinute, durationMinutes)
            root.moved = false
        }
        onCanceled: root.moved = false
    }

    Rectangle {
        visible: creationArea.pressed && root.moved
        x: 4
        width: parent.width - 8
        y: (root.selectionStartMinute / 60 - root.firstHour)
           * root.pixelsPerHour
        height: Math.max(4, root.selectionDurationMinutes / 60
                         * root.pixelsPerHour)
        radius: 6
        color: Theme.alpha(Theme.accent, 0.17)
        border.width: 1
        border.color: Theme.alpha(Theme.accent, 0.72)
        z: 1

        Text {
            anchors.centerIn: parent
            visible: parent.height >= implicitHeight + 6
            text: root.timeLabel(root.selectionStartMinute) + " – "
                  + root.timeLabel(root.selectionStartMinute
                                   + root.selectionDurationMinutes)
            color: Theme.text
            font.pixelSize: Theme.microFontSize
        }
    }

    function minuteAt(yPosition) {
        const minute = (yPosition / pixelsPerHour + firstHour) * 60
        return Math.max(firstHour * 60,
                        Math.min(lastHour * 60 - 15,
                                 Math.round(minute / 15) * 15))
    }

    function timeLabel(minute) {
        const bounded = Math.max(0, Math.min(1440, minute))
        const dateValue = new Date(2000, 0, 1, Math.floor(bounded / 60),
                                   bounded % 60)
        return Qt.formatTime(dateValue, "HH:mm")
    }
}
