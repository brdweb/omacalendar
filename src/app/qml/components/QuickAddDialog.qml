pragma ComponentBehavior: Bound
// App is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// One line such as "Lunch with Sam fri 12:30" becomes a pre-filled draft in the
// event editor. Nothing is saved from here.
Dialog {
    id: root

    property var draft: ({})
    signal draftAccepted(var draft)

    anchors.centerIn: Overlay.overlay
    width: Math.min(560, Overlay.overlay ? Overlay.overlay.width - 48 : 560)
    modal: true
    title: qsTr("Quick add")
    standardButtons: Dialog.Cancel
    closePolicy: Popup.CloseOnEscape

    function openEmpty() {
        input.text = ""
        draft = ({})
        open()
        input.forceActiveFocus()
    }

    function parse(text) {
        if (typeof App.parseQuickAdd !== "function" || text.trim().length === 0)
            return ({})
        return App.parseQuickAdd(text)
    }

    function minuteText(minutes) {
        return String(Math.floor(minutes / 60)).padStart(2, "0") + ":"
                + String(minutes % 60).padStart(2, "0")
    }

    // What the editor will be pre-filled with, so the user can check the
    // reading before continuing.
    function summary(value) {
        if (!value.title && !value.date && value.startMinute < 0)
            return ""
        const parts = [value.title || qsTr("Untitled event")]
        if (value.date) {
            const first = new Date(value.date + "T00:00:00")
            let dateText = Qt.formatDate(first, "ddd MMM d")
            if (value.allDay && value.endDate) {
                const last = new Date(value.endDate + "T00:00:00")
                last.setDate(last.getDate() - 1)
                if (last > first)
                    dateText += " – " + Qt.formatDate(last, "ddd MMM d")
            }
            parts.push(dateText)
        }
        if (value.startMinute >= 0) {
            let timeText = minuteText(value.startMinute)
            if (value.durationMinutes > 0)
                timeText += " – " + minuteText((value.startMinute + value.durationMinutes)
                                               % (24 * 60))
            parts.push(timeText)
        } else if (value.allDay) {
            parts.push(qsTr("all day"))
        }
        if (value.recurrenceRule)
            parts.push(qsTr("repeats"))
        if (value.location)
            parts.push("@ " + value.location)
        return parts.join("  ·  ")
    }

    function accept() {
        const value = parse(input.text)
        if (!value.title && !value.date && !(value.startMinute >= 0))
            return
        close()
        draftAccepted(value)
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingSM

        AppTextField {
            id: input
            objectName: "quickAddInput"
            Layout.fillWidth: true
            placeholderText: qsTr("Lunch with Sam fri 12:30 @ Café")
            accessibleName: qsTr("Describe the event")
            onTextEdited: root.draft = root.parse(text)
            onAccepted: root.accept()
        }
        Text {
            objectName: "quickAddPreview"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: root.summary(root.draft) || qsTr("Try a title with a day, a time or a range, like \"Trip Oct 3-6\" or \"Standup every weekday 9am 15m\".")
            color: root.summary(root.draft) ? Theme.text : Theme.mutedText
            wrapMode: Text.Wrap
            font.pixelSize: Theme.smallFontSize
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Continue in editor")
                primary: true
                enabled: input.text.trim().length > 0
                onClicked: root.accept()
            }
        }
    }
}
