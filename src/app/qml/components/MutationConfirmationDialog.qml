pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

Dialog {
    id: root

    property var eventData: ({})
    property var contextData: ({})
    property var baseOptions: ({})
    property string actionLabel: qsTr("Change event")
    property bool futureScopeSupported: false
    readonly property bool recurring: Boolean(eventData.recurrenceRule
                                               || eventData.recurrenceId)
    readonly property bool hasGuests: hasExternalGuests(eventData.attendees || [])
    readonly property bool needsChoice: recurring || hasGuests
    readonly property bool canSubmit: (!recurring || recurrenceScopeBox.currentValue)
                                      && (!hasGuests || guestPolicyBox.currentValue)

    signal confirmed(var contextData, var mutationOptions)

    anchors.centerIn: Overlay.overlay
    width: Math.min(480, Overlay.overlay ? Overlay.overlay.width - 48 : 480)
    modal: true
    title: actionLabel + qsTr("?")
    standardButtons: Dialog.Cancel
    closePolicy: Popup.CloseOnEscape

    function hasExternalGuests(attendees) {
        for (let index = 0; index < attendees.length; ++index) {
            const attendee = attendees[index]
            if (typeof attendee === "string" || attendee.self !== true)
                return true
        }
        return false
    }

    function needsChoiceFor(value) {
        const candidate = value || ({})
        return Boolean(candidate.recurrenceRule || candidate.recurrenceId)
                || hasExternalGuests(candidate.attendees || [])
    }

    function recurrenceChoices() {
        const choices = [{"text": qsTr("Choose recurrence scope…"), "value": ""}]
        if (String(eventData.recurrenceId || "").length > 0) {
            choices.push({"text": qsTr("This occurrence"), "value": "occurrence"})
            if (futureScopeSupported)
                choices.push({"text": qsTr("This and future occurrences"), "value": "future"})
        }
        choices.push({"text": qsTr("Entire series"), "value": "series"})
        return choices
    }

    function openFor(value, label, context, options, supportsFuture) {
        eventData = value || ({})
        actionLabel = String(label || qsTr("Change event"))
        contextData = context || ({})
        baseOptions = options || ({})
        futureScopeSupported = supportsFuture === true
        recurrenceScopeBox.currentIndex = 0
        guestPolicyBox.currentIndex = 0
        open()
        if (recurring)
            recurrenceScopeBox.forceActiveFocus()
        else if (hasGuests)
            guestPolicyBox.forceActiveFocus()
    }

    function submit() {
        if (!canSubmit)
            return
        const options = Object.assign({}, baseOptions)
        options.recurrenceScope = recurring ? recurrenceScopeBox.currentValue
                                            : "series"
        options.guestNotificationPolicy = hasGuests ? guestPolicyBox.currentValue
                                                    : "none"
        const context = contextData
        close()
        confirmed(context, options)
    }

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            Layout.fillWidth: true
            text: root.eventData.summary || qsTr("Untitled event")
            color: Theme.text
            font.pixelSize: Theme.fontSize
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }
        Text {
            visible: root.recurring
            Layout.fillWidth: true
            text: qsTr("Choose exactly which part of the recurring event to change.")
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
            wrapMode: Text.Wrap
        }
        AppComboBox {
            id: recurrenceScopeBox
            objectName: "mutationRecurrenceScope"
            visible: root.recurring
            Layout.fillWidth: true
            model: root.recurrenceChoices()
            textRole: "text"
            valueRole: "value"
            Accessible.name: qsTr("Recurring event scope")
        }
        Text {
            visible: root.hasGuests
            Layout.fillWidth: true
            text: qsTr("This event has guests. Choose whether the provider should notify them.")
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
            wrapMode: Text.Wrap
        }
        AppComboBox {
            id: guestPolicyBox
            objectName: "mutationGuestPolicy"
            visible: root.hasGuests
            Layout.fillWidth: true
            model: [
                {"text": qsTr("Choose guest notification policy…"), "value": ""},
                {"text": qsTr("Do not notify guests"), "value": "none"},
                {"text": qsTr("Notify external guests only"), "value": "externalOnly"},
                {"text": qsTr("Notify all guests"), "value": "all"}
            ]
            textRole: "text"
            valueRole: "value"
            Accessible.name: qsTr("Guest notification policy")
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "confirmMutationButton"
                text: root.actionLabel
                primary: true
                enabled: root.canSubmit
                onClicked: root.submit()
            }
        }
    }
}
