pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import OmaCalendar

// A short-lived note after a change the user may want back, such as
// "Event deleted · Undo". It hides before the daemon's 10-second window for
// undoing a delete closes.
Rectangle {
    id: root

    property string message: ""
    property bool undoable: false
    readonly property bool shown: hideTimer.running
    signal undoRequested()

    function show(text, canUndo) {
        message = text
        undoable = canUndo
        hideTimer.restart()
    }

    function dismiss() {
        hideTimer.stop()
    }

    objectName: "undoToast"
    visible: shown
    opacity: shown ? 1 : 0
    implicitWidth: toastRow.implicitWidth + 2 * Theme.spacingMD
    implicitHeight: 40
    radius: Theme.radiusLG
    color: Theme.surfaceAlt
    border.color: Theme.border
    Accessible.role: Accessible.AlertMessage
    Accessible.name: message

    Behavior on opacity { NumberAnimation { duration: 120 } }

    Timer {
        id: hideTimer
        interval: 8000
    }

    RowLayout {
        id: toastRow
        anchors.centerIn: parent
        spacing: Theme.spacingMD
        Text {
            objectName: "undoToastMessage"
            textFormat: Text.PlainText
            text: root.message
            color: Theme.text
            font.pixelSize: Theme.smallFontSize
        }
        AppButton {
            objectName: "undoToastAction"
            visible: root.undoable
            compact: true
            quiet: true
            text: qsTr("Undo")
            toolTipText: qsTr("Undo  Ctrl+Z")
            onClicked: {
                root.dismiss()
                root.undoRequested()
            }
        }
        AppButton {
            compact: true
            quiet: true
            iconText: "×"
            toolTipText: qsTr("Dismiss")
            onClicked: root.dismiss()
        }
    }
}
