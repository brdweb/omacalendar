pragma ComponentBehavior: Bound
// App is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Guest chips for the event editor. Each committed address becomes a chip that
// keeps the provider's attendee fields, shows the guest's response, and can be
// removed; typing offers addresses already seen in the local cache.
ColumnLayout {
    id: root

    property var attendees: []
    property string organizerEmail: ""
    property bool editable: true
    property var suggestions: []
    property string invalidEntry: ""
    readonly property bool hasGuests: attendees.length > 0
                                      || input.text.trim().length > 0

    spacing: Theme.spacingXS

    function load(values) {
        attendees = (values || []).filter(function(value) {
            return value && String(value.email || "").trim().length > 0
        }).map(function(value) { return Object.assign({}, value) })
        input.text = ""
        invalidEntry = ""
        suggestions = []
    }

    function isEmail(value) {
        return /^[^\s@,;<>]+@[^\s@,;<>]+\.[^\s@,;<>]+$/.test(value)
    }

    function hasAttendee(email) {
        const wanted = email.toLowerCase()
        return attendees.some(function(value) {
            return String(value.email || "").toLowerCase() === wanted
        })
    }

    // Moves typed addresses into chips. Invalid entries stay in the input and
    // are reported, so nothing the user typed is silently dropped.
    function commitInput() {
        const tokens = input.text.split(/[\n,;\s]+/)
        const next = attendees.slice()
        const rejected = []
        for (let index = 0; index < tokens.length; ++index) {
            const email = tokens[index].trim()
            if (email.length === 0)
                continue
            if (!isEmail(email)) {
                rejected.push(email)
                continue
            }
            const wanted = email.toLowerCase()
            if (!next.some(function(value) {
                    return String(value.email || "").toLowerCase() === wanted }))
                next.push({"email": email})
        }
        attendees = next
        input.text = rejected.join(", ")
        invalidEntry = rejected.length > 0 ? rejected[0] : ""
        suggestions = []
        return rejected.length === 0
    }

    function addSuggestion(contact) {
        if (!hasAttendee(String(contact.email))) {
            const value = {"email": String(contact.email)}
            if (contact.displayName)
                value.displayName = String(contact.displayName)
            attendees = attendees.concat([value])
        }
        input.text = ""
        suggestions = []
        input.forceActiveFocus()
    }

    function removeAt(index) {
        const next = attendees.slice()
        next.splice(index, 1)
        attendees = next
    }

    // The attendees to save, including a valid address still being typed.
    function result() {
        commitInput()
        return attendees
    }

    function validationError() {
        return invalidEntry.length > 0
                ? qsTr("Check the guest address \"%1\".").arg(invalidEntry) : ""
    }

    function responseOf(value) {
        const status = String(value.responseStatus || value.partstat || "").toLowerCase()
        if (status === "accepted")
            return {"text": qsTr("Accepted"), "color": Theme.success}
        if (status === "declined")
            return {"text": qsTr("Declined"), "color": Theme.danger}
        if (status === "tentative")
            return {"text": qsTr("Maybe"), "color": Theme.warning}
        if (status === "needsaction" || status === "needs-action")
            return {"text": qsTr("Awaiting reply"), "color": Theme.mutedText}
        return {"text": "", "color": Theme.mutedText}
    }

    function requestSuggestions() {
        const prefix = input.text.trim()
        if (prefix.length < 2 || /[,;\s]/.test(prefix)) {
            suggestions = []
            return
        }
        if (typeof App.suggestContacts === "function")
            App.suggestContacts(prefix)
    }

    Connections {
        target: App
        ignoreUnknownSignals: true
        function onContactSuggestionsReady(prefix, contacts) {
            if (prefix !== input.text.trim())
                return
            root.suggestions = contacts.filter(function(contact) {
                return !root.hasAttendee(String(contact.email || ""))
            })
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spacingXS
        visible: root.attendees.length > 0

        Repeater {
            model: root.attendees
            delegate: Rectangle {
                id: chip
                required property var modelData
                required property int index
                readonly property var response: root.responseOf(modelData)
                readonly property bool organizer: modelData.organizer === true
                    || (root.organizerEmail.length > 0
                        && String(modelData.email).toLowerCase()
                           === root.organizerEmail.toLowerCase())
                objectName: "attendeeChip-" + index
                implicitWidth: chipRow.implicitWidth + 16
                implicitHeight: 30
                radius: height / 2
                color: Theme.surfaceAlt
                border.color: Theme.border
                Accessible.role: Accessible.StaticText
                Accessible.name: [modelData.displayName || modelData.email,
                                  organizer ? qsTr("organizer") : "",
                                  response.text].filter(Boolean).join(", ")

                RowLayout {
                    id: chipRow
                    anchors.verticalCenter: parent.verticalCenter
                    x: 8
                    spacing: Theme.spacingXS
                    Text {
                        textFormat: Text.PlainText
                        text: chip.modelData.displayName || chip.modelData.email
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                        elide: Text.ElideRight
                        Layout.maximumWidth: 220
                    }
                    Text {
                        textFormat: Text.PlainText
                        visible: chip.organizer
                        text: qsTr("Organizer")
                        color: Theme.accent
                        font.pixelSize: Theme.microFontSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        textFormat: Text.PlainText
                        visible: chip.response.text.length > 0
                        text: chip.response.text
                        color: chip.response.color
                        font.pixelSize: Theme.microFontSize
                    }
                    AppButton {
                        visible: root.editable
                        compact: true
                        quiet: true
                        iconText: "×"
                        implicitHeight: 22
                        toolTipText: qsTr("Remove %1").arg(chip.modelData.email)
                        onClicked: root.removeAt(chip.index)
                    }
                }
            }
        }
    }

    AppTextField {
        id: input
        objectName: "attendeeInput"
        Layout.fillWidth: true
        enabled: root.editable
        placeholderText: root.attendees.length > 0 ? qsTr("Add another guest")
                                                   : qsTr("Add guests by email")
        accessibleName: qsTr("Event guests")
        onTextEdited: {
            root.invalidEntry = ""
            if (/[,;]\s*$/.test(text))
                root.commitInput()
            else
                root.requestSuggestions()
        }
        onAccepted: root.commitInput()
        onEditingFinished: {
            if (text.trim().length > 0 && root.suggestions.length === 0)
                root.commitInput()
        }
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Backspace && text.length === 0
                    && root.attendees.length > 0) {
                root.removeAt(root.attendees.length - 1)
                event.accepted = true
            } else if (event.key === Qt.Key_Down && suggestionList.count > 0) {
                suggestionList.forceActiveFocus()
                event.accepted = true
            }
        }
    }

    ListView {
        id: suggestionList
        objectName: "attendeeSuggestions"
        visible: count > 0 && root.editable
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(count, 5) * 34
        clip: true
        model: root.suggestions
        keyNavigationEnabled: true
        delegate: ItemDelegate {
            id: suggestion
            required property var modelData
            required property int index
            width: suggestionList.width
            height: 34
            highlighted: suggestionList.activeFocus && suggestionList.currentIndex === index
            text: modelData.displayName
                  ? modelData.displayName + "  ·  " + modelData.email
                  : modelData.email
            Accessible.name: text
            onClicked: root.addSuggestion(modelData)
            Keys.onReturnPressed: root.addSuggestion(modelData)
            Keys.onEnterPressed: root.addSuggestion(modelData)
        }
    }

    Text {
        visible: root.invalidEntry.length > 0
        Layout.fillWidth: true
        textFormat: Text.PlainText
        text: root.validationError()
        color: Theme.danger
        font.pixelSize: Theme.smallFontSize
        wrapMode: Text.Wrap
    }
}
