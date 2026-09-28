pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

ItemDelegate {
    id: root
    required property var eventData
    property bool showDate: false
    property string continuationText: ""
    property string timeFormat: "system"
    property bool selected: false
    // Search results: words to highlight in the title, and a click that jumps
    // to the event (double-click still edits it).
    property string highlight: ""
    property bool jumpOnClick: false
    readonly property bool generatedInstance: Boolean(eventData.recurrenceRule
                                                       && eventData.recurrenceId)
    readonly property string operationState: eventData.operationState || ""
    readonly property string stateLabel: {
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
    readonly property string displayStateLabel: {
        if (stateLabel === "Conflict")
            return qsTr("Conflict")
        if (stateLabel === "Retrying")
            return qsTr("Retrying")
        if (stateLabel === "Failed")
            return qsTr("Failed")
        if (stateLabel === "Pending")
            return qsTr("Pending")
        if (stateLabel === "Read only")
            return qsTr("Read only")
        return ""
    }
    signal editRequested(var eventData)
    signal jumpRequested(var eventData)

    width: ListView.view ? ListView.view.width : 400
    height: Math.max(68, details.implicitHeight + 24)
    padding: 0
    Accessible.name: eventData.summary || qsTr("Untitled event")
    Accessible.description: eventDetails.text
                            + (displayStateLabel.length > 0 ? " · " + displayStateLabel : "")
    Accessible.role: Accessible.Button
    onClicked: {
        // Wait out the double-click interval: jumping closes the search panel,
        // which would swallow the second click of a double-click.
        if (jumpOnClick)
            jumpDelay.restart()
        else
            editRequested(eventData)
    }
    onDoubleClicked: {
        if (jumpOnClick) {
            jumpDelay.stop()
            editRequested(eventData)
        }
    }
    Keys.onReturnPressed: jumpOnClick ? jumpRequested(eventData) : editRequested(eventData)
    Keys.onEnterPressed: jumpOnClick ? jumpRequested(eventData) : editRequested(eventData)

    Timer {
        id: jumpDelay
        objectName: "eventRowJumpDelay"
        interval: Application.styleHints.mouseDoubleClickInterval
        onTriggered: root.jumpRequested(root.eventData)
    }
    ToolTip.visible: generatedInstance && hovered
    ToolTip.text: qsTr("Recurring event — choose an occurrence scope when editing")
    ToolTip.delay: 450

    background: Rectangle {
        radius: Theme.radiusLG
        color: root.selected ? Theme.alpha(Theme.accent, 0.12)
                             : root.hovered ? Theme.surfaceAlt : Theme.surface
        border.width: root.selected || root.activeFocus ? 2 : 1
        border.color: root.selected || root.activeFocus
                      ? Theme.focus
                      : Theme.alpha(Theme.text, root.hovered ? 0.18 : 0.08)
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
                objectName: "eventRowTitle"
                // Styled only when highlighting; the title is escaped first so
                // provider markup still shows literally.
                textFormat: root.highlight.trim().length > 0 ? Text.StyledText
                                                             : Text.PlainText
                Layout.fillWidth: true
                text: root.highlight.trim().length > 0
                      ? root.highlightedText(root.eventData.summary || qsTr("Untitled event"))
                      : root.eventData.summary || qsTr("Untitled event")
                color: Theme.text
                font.pixelSize: Theme.fontSize
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                id: eventDetails
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: {
                    const prefix = root.showDate
                            ? Qt.formatDate(root.eventDate(root.eventData), "ddd, MMM d") + "  ·  "
                            : ""
                    if (root.eventData.allDay)
                        return prefix + qsTr("All day")
                    const start = new Date(root.eventData.displayStartLocal
                                           || root.eventData.startUtc)
                    const end = new Date(root.eventData.displayEndLocal
                                         || root.eventData.endUtc)
                    return prefix + Qt.formatTime(start, root.timePattern()) + " – "
                           + Qt.formatTime(end, root.timePattern())
                           + (root.eventData.location ? "  ·  " + root.eventData.location : "")
                           + (root.generatedInstance ? qsTr("  ·  Recurring") : "")
                }
                color: Theme.mutedText
                font.pixelSize: Theme.smallFontSize
                elide: Text.ElideRight
            }
            Text {
                textFormat: Text.PlainText
                visible: root.continuationText.length > 0
                         || Boolean(root.eventData.organizer
                                    && (root.eventData.organizer.displayName
                                        || root.eventData.organizer.email))
                Layout.fillWidth: true
                text: root.continuationText.length > 0
                      ? root.continuationText
                      : qsTr("Organized by ") + (root.eventData.organizer.displayName
                                           || root.eventData.organizer.email)
                color: Theme.alpha(Theme.mutedText, 0.82)
                font.pixelSize: Theme.microFontSize
                elide: Text.ElideRight
            }
        }
        StatusBadge {
            visible: root.stateLabel.length > 0
            Layout.rightMargin: 13
            text: root.displayStateLabel
            tone: root.stateLabel === "Conflict" || root.stateLabel === "Failed"
                  ? "danger"
                                                  : root.stateLabel === "Retrying"
                                                    ? "warning" : "info"
        }
    }

    function escapeHtml(value) {
        return String(value).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }

    // Wraps each case-insensitive occurrence of a highlight word in the
    // accent colour. Matching runs on the raw text; every piece is escaped.
    function highlightedText(value) {
        const source = String(value)
        const words = highlight.toLowerCase().split(/\s+/).filter(function(word) {
            return word.length > 0
        })
        const lower = source.toLowerCase()
        const marks = new Array(source.length).fill(false)
        for (const word of words) {
            let from = lower.indexOf(word)
            while (from >= 0) {
                for (let index = from; index < from + word.length; ++index)
                    marks[index] = true
                from = lower.indexOf(word, from + word.length)
            }
        }
        let result = ""
        let index = 0
        while (index < source.length) {
            let end = index
            while (end < source.length && marks[end] === marks[index])
                ++end
            const piece = escapeHtml(source.slice(index, end))
            result += marks[index] ? "<font color=\"" + Theme.accent + "\"><b>" + piece
                                     + "</b></font>"
                                   : piece
            index = end
        }
        return result
    }

    function eventDate(value) {
        return value.allDay ? new Date(value.startDate + "T00:00:00")
                            : new Date(value.displayStartLocal || value.startUtc)
    }

    function timePattern() {
        if (timeFormat === "24h")
            return "HH:mm"
        if (timeFormat === "12h")
            return "h:mm AP"
        return Qt.locale().timeFormat(Locale.ShortFormat)
    }
}
