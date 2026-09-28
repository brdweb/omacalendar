pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

Drawer {
    id: root

    property var accounts: []
    property var calendars: []
    property var calendarSets: []
    property var accountsModel: null
    property var calendarsModel: null
    property var calendarSetsModel: null
    readonly property var effectiveAccountsModel: accountsModel || accounts
    readonly property var effectiveCalendarsModel: calendarsModel || calendars
    readonly property var effectiveCalendarSetsModel: calendarSetsModel
                                                       || calendarSets
    property bool connected: false
    property bool busy: false
    property string statusText: ""
    property string lastError: ""
    property var preferences: ({})
    property string systemTimeZoneId: "UTC"
    property var availableTimeZoneIds: ["UTC"]
    property bool bundledGoogleOAuthAvailable: false
    property bool googleOAuthConfigured: false
    readonly property var timeZoneOptions: buildTimeZoneOptions()
    readonly property var defaultCalendarOptions: buildDefaultCalendarOptions()

    // Inline add-account feedback. The global banner is easy to miss while the
    // drawer covers it, so each form reports its own outcome.
    property string submissionForm: ""
    property string submissionMessage: ""
    property string submissionTone: "info"
    property bool submissionPending: false
    property int submissionAccountCount: 0
    // Set between confirming a calendar deletion and the model catching up.
    property string deletingCalendarId: ""

    // Dialogs the tab components open.
    readonly property var credentialDialogRef: credentialDialog
    readonly property var removeConfirmRef: removeConfirm
    readonly property var calendarSetDialogRef: calendarSetDialog
    readonly property var localCalendarDialogRef: localCalendarDialog
    readonly property var localCalendarRemoveConfirmRef: localCalendarRemoveConfirm

    signal connectGoogleRequested(string displayName)
    signal connectGoogleClientRequested(string clientId, string displayName)
    signal connectGoogleCredentialsRequested(string displayName)
    signal addCalDavRequested(string endpoint, string username,
                              string password, string displayName)
    signal addLocalCalendarRequested(string name, string color, bool muteAlerts)
    signal removeCalendarRequested(string calendarId)
    signal addIcsSubscriptionRequested(string url, string username,
                                       string password, string displayName)
    signal removeAccountRequested(string accountId, bool removeCachedData)
    signal reauthorizeAccountRequested(string accountId)
    signal updateAccountCredentialsRequested(string accountId, string username,
                                             string password)
    signal syncAccountRequested(string accountId)
    signal calendarPreferenceChanged(string calendarId, string key, var value)
    signal preferenceChanged(string key, var value)
    signal diagnosticsRequested()
    signal importIcsRequested()
    signal exportIcsRequested()
    signal upsertCalendarSetRequested(var calendarSet)
    signal removeCalendarSetRequested(string calendarSetId)

    function openAccounts() {
        settingsTabs.currentIndex = 0
        open()
    }

    function beginSubmission(formId) {
        submissionForm = String(formId)
        submissionMessage = ""
        submissionTone = "info"
        submissionPending = true
        submissionAccountCount = accounts.length
        submissionTimer.restart()
    }

    function finishSubmission(succeeded, message) {
        if (submissionForm.length === 0)
            return
        submissionPending = false
        submissionTone = succeeded ? "success" : "danger"
        submissionMessage = String(message || "")
        submissionTimer.restart()
    }

    function clearSubmission() {
        submissionForm = ""
        submissionMessage = ""
        submissionPending = false
        submissionTone = "info"
    }

    function submissionStatusText() {
        if (!submissionPending)
            return submissionMessage
        return busy && statusText.length > 0 ? statusText : qsTr("Connecting…")
    }

    function submissionToneColor() {
        if (submissionTone === "danger")
            return Theme.danger
        if (submissionTone === "success")
            return Theme.success
        return Theme.mutedText
    }

    onAccountsChanged: {
        if (submissionPending && accounts.length > submissionAccountCount) {
            finishSubmission(true,
                             qsTr("Connected. Calendars are syncing in the background."))
        }
    }

    onLastErrorChanged: {
        if (submissionPending && lastError.length > 0)
            finishSubmission(false, lastError)
    }

    onCalendarsChanged: deletingCalendarId = ""

    Timer {
        id: submissionTimer
        // Long window while the provider round-trip is in flight, short window
        // once the form has reported its outcome.
        interval: root.submissionPending ? 45000 : 6000
        running: root.submissionForm.length > 0
        onTriggered: root.clearSubmission()
    }

    Timer {
        id: deletingGuard
        interval: 8000
        running: root.deletingCalendarId.length > 0
        onTriggered: root.deletingCalendarId = ""
    }

    function buildTimeZoneOptions() {
        const result = [{"text": qsTr("System default — ") + systemTimeZoneId,
                         "value": ""}]
        for (let index = 0; index < availableTimeZoneIds.length; ++index) {
            const id = String(availableTimeZoneIds[index])
            if (id.length > 0)
                result.push({"text": id, "value": id})
        }
        return result
    }

    function displayTimeZoneIndex() {
        const selected = String(preferences.displayTimeZone || "")
        for (let index = 0; index < timeZoneOptions.length; ++index) {
            if (String(timeZoneOptions[index].value) === selected)
                return index
        }
        return 0
    }

    function buildDefaultCalendarOptions() {
        const result = []
        for (let index = 0; index < calendars.length; ++index) {
            const calendar = calendars[index]
            if (calendar && calendar.enabled !== false
                    && calendar.readOnly !== true) {
                result.push({"text": String(calendar.name || qsTr("Calendar")),
                             "value": String(calendar.id || "")})
            }
        }
        return result
    }

    function defaultCalendarIndex() {
        const selected = String(preferences.defaultCalendarId || "")
        for (let index = 0; index < defaultCalendarOptions.length; ++index) {
            if (String(defaultCalendarOptions[index].value) === selected)
                return index
        }
        return defaultCalendarOptions.length > 0 ? 0 : -1
    }

    function reorderCalendar(sourceId, targetId, placeAfter) {
        sourceId = String(sourceId || "")
        targetId = String(targetId || "")
        if (!sourceId || !targetId || sourceId === targetId)
            return

        const ordered = calendars.slice().sort(function(left, right) {
            return Number(left.position || 0) - Number(right.position || 0)
        })
        let sourceIndex = -1
        for (let index = 0; index < ordered.length; ++index) {
            if (String(ordered[index].id) === sourceId) {
                sourceIndex = index
                break
            }
        }
        if (sourceIndex < 0)
            return

        const moved = ordered.splice(sourceIndex, 1)[0]
        let targetIndex = -1
        for (let index = 0; index < ordered.length; ++index) {
            if (String(ordered[index].id) === targetId) {
                targetIndex = index
                break
            }
        }
        if (targetIndex < 0)
            return
        ordered.splice(targetIndex + (placeAfter ? 1 : 0), 0, moved)
        commitCalendarOrder(ordered)
    }

    function orderedCalendarList() {
        return calendars.slice().sort(function(left, right) {
            return Number(left.position || 0) - Number(right.position || 0)
        })
    }

    function commitCalendarOrder(ordered) {
        for (let index = 0; index < ordered.length; ++index) {
            if (Number(ordered[index].position || 0) !== index)
                calendarPreferenceChanged(String(ordered[index].id),
                                          "position", index)
        }
    }

    // Keyboard equivalent of the drag handle; commits through the same
    // position-update path the drop handler uses.
    function moveCalendarByOffset(calendarId, direction) {
        const id = String(calendarId || "")
        if (id.length === 0)
            return
        const ordered = orderedCalendarList()
        let sourceIndex = -1
        for (let index = 0; index < ordered.length; ++index) {
            if (String(ordered[index].id) === id) {
                sourceIndex = index
                break
            }
        }
        const targetIndex = sourceIndex + direction
        if (sourceIndex < 0 || targetIndex < 0 || targetIndex >= ordered.length)
            return
        const moved = ordered[sourceIndex]
        ordered[sourceIndex] = ordered[targetIndex]
        ordered[targetIndex] = moved
        commitCalendarOrder(ordered)
    }

    function calendarOrderIndex(calendarId) {
        const ordered = orderedCalendarList()
        for (let index = 0; index < ordered.length; ++index) {
            if (String(ordered[index].id) === String(calendarId))
                return index
        }
        return -1
    }

    edge: Qt.RightEdge
    // Keep the 560px cap on wide windows, but on a narrow window take the room
    // the forms need while leaving a strip of the calendar visible behind.
    width: Overlay.overlay
           ? Math.min(560, Math.max(Overlay.overlay.width * 0.52,
                                    Overlay.overlay.width - 220))
           : 560
    height: Overlay.overlay ? Overlay.overlay.height : 760
    modal: true

    enter: Transition {
        NumberAnimation {
            property: "position"
            to: 1.0
            duration: 180
            easing.type: Easing.OutCubic
        }
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 180 }
    }
    exit: Transition {
        NumberAnimation {
            property: "position"
            to: 0.0
            duration: 150
            easing.type: Easing.InCubic
        }
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 150 }
    }

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            spacing: Theme.spacingSM
            AppCloseButton {
                toolTipText: qsTr("Close settings")
                onClicked: root.close()
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    textFormat: Text.PlainText
                    text: qsTr("Accounts & settings")
                    color: Theme.text
                    font.pixelSize: Theme.titleFontSize
                    font.weight: Font.Bold
                }
                Text {
                    textFormat: Text.PlainText
                    text: qsTr("Credentials stay in your desktop keyring.")
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                }
            }
        }

        TabBar {
            id: settingsTabs
            Layout.fillWidth: true
            background: Rectangle { color: Theme.darkBackground }
            AppTabButton { text: qsTr("Accounts") }
            AppTabButton { text: qsTr("Calendars") }
            AppTabButton { text: qsTr("Preferences") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: settingsTabs.currentIndex

            SettingsAccountsTab {
                drawer: root
            }

            SettingsCalendarsTab {
                drawer: root
            }

            SettingsPreferencesTab {
                drawer: root
            }
        }
    }

    Dialog {
        id: localCalendarRemoveConfirm
        objectName: "deleteLocalCalendarConfirm"
        property var calendarData: ({})
        anchors.centerIn: Overlay.overlay
        width: Math.min(430, Overlay.overlay ? Overlay.overlay.width - 48 : 430)
        modal: true
        title: qsTr("Delete calendar?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        function openFor(value) {
            calendarData = value || ({})
            open()
        }
        onAccepted: root.removeCalendarRequested(
                        String(calendarData.id || ""))
        contentItem: Text {
            textFormat: Text.PlainText
            width: 380
            text: {
                const value = localCalendarRemoveConfirm.calendarData
                const provider = String((value.capabilities || {}).provider
                                        || root.accountProvider(value.accountId))
                const location = provider === "google"
                        ? qsTr(" from Google Calendar") : qsTr(" from this device")
                return qsTr("Permanently delete ") + (value.name || qsTr("this calendar"))
                        + location + qsTr(" and all of its events? This cannot be undone.")
            }
            color: Theme.text
            wrapMode: Text.Wrap
        }
    }

    Dialog {
        id: calendarSetDialog
        objectName: "calendarSetDialog"
        property var setData: ({})
        property var selectedIds: []
        property string defaultCalendarId: ""
        property int selectionRevision: 0
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 48 : 460)
        modal: true
        title: setData.id ? qsTr("Edit calendar set") : qsTr("New calendar set")
        standardButtons: Dialog.Cancel

        function openNew() {
            setData = ({})
            setName.text = ""
            selectedIds = []
            defaultCalendarId = ""
            ++selectionRevision
            open()
        }

        function openExisting(value) {
            setData = value || ({})
            setName.text = setData.name || ""
            selectedIds = (setData.calendarIds || []).slice()
            defaultCalendarId = String(setData.defaultCalendarId || "")
            ++selectionRevision
            open()
        }

        function selectionIndex(calendarId) {
            selectionRevision
            return selectedIds.indexOf(String(calendarId))
        }

        function selectedCalendars() {
            selectionRevision
            const values = []
            for (let selectedIndex = 0; selectedIndex < selectedIds.length;
                 ++selectedIndex) {
                for (let calendarIndex = 0; calendarIndex < root.calendars.length;
                     ++calendarIndex) {
                    if (String(root.calendars[calendarIndex].id)
                            === String(selectedIds[selectedIndex])) {
                        values.push(root.calendars[calendarIndex])
                        break
                    }
                }
            }
            return values
        }

        function orderedCalendars() {
            selectionRevision
            const values = selectedCalendars()
            for (let calendarIndex = 0; calendarIndex < root.calendars.length;
                 ++calendarIndex) {
                const calendar = root.calendars[calendarIndex]
                if (selectedIds.indexOf(String(calendar.id)) < 0)
                    values.push(calendar)
            }
            return values
        }

        function toggleCalendar(calendarId, checked) {
            const id = String(calendarId)
            const values = selectedIds.slice()
            const index = values.indexOf(id)
            if (checked && index < 0) {
                values.push(id)
            } else if (!checked && index >= 0) {
                values.splice(index, 1)
            } else {
                return
            }
            selectedIds = values
            if (values.indexOf(defaultCalendarId) < 0)
                defaultCalendarId = values.length > 0 ? values[0] : ""
            ++selectionRevision
        }

        function moveCalendar(calendarId, direction) {
            const values = selectedIds.slice()
            const index = values.indexOf(String(calendarId))
            const target = index + direction
            if (index < 0 || target < 0 || target >= values.length)
                return
            const moved = values[index]
            values[index] = values[target]
            values[target] = moved
            selectedIds = values
            ++selectionRevision
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingSM
            AppTextField {
                id: setName
                Layout.fillWidth: true
                placeholderText: qsTr("Set name")
                accessibleName: qsTr("Calendar set name")
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: qsTr("Select calendars. Use the arrows to control their order.")
                color: Theme.mutedText
                font.pixelSize: Theme.smallFontSize
                wrapMode: Text.Wrap
            }
            ColumnLayout {
                Layout.fillWidth: true
                Repeater {
                    model: calendarSetDialog.orderedCalendars()
                    delegate: RowLayout {
                        id: membershipRow
                        objectName: "calendarSetMembership-" + String(modelData.id)
                        required property var modelData
                        Layout.fillWidth: true
                        readonly property int selectedIndex:
                            calendarSetDialog.selectionIndex(modelData.id)
                        AppCheckBox {
                            Layout.fillWidth: true
                            text: membershipRow.modelData.name || qsTr("Calendar")
                            checked: membershipRow.selectedIndex >= 0
                            onToggled: calendarSetDialog.toggleCalendar(
                                           membershipRow.modelData.id, checked)
                        }
                        AppButton {
                            objectName: "calendarSetMoveEarlier-" + String(
                                            membershipRow.modelData.id)
                            visible: membershipRow.selectedIndex >= 0
                            iconText: "↑"
                            compact: true
                            quiet: true
                            enabled: membershipRow.selectedIndex > 0
                            toolTipText: qsTr("Move earlier")
                            onClicked: calendarSetDialog.moveCalendar(
                                           membershipRow.modelData.id, -1)
                        }
                        AppButton {
                            objectName: "calendarSetMoveLater-" + String(
                                            membershipRow.modelData.id)
                            visible: membershipRow.selectedIndex >= 0
                            iconText: "↓"
                            compact: true
                            quiet: true
                            enabled: membershipRow.selectedIndex
                                     < calendarSetDialog.selectedIds.length - 1
                            toolTipText: qsTr("Move later")
                            onClicked: calendarSetDialog.moveCalendar(
                                           membershipRow.modelData.id, 1)
                        }
                    }
                }
            }
            AppComboBox {
                id: setDefaultCalendar
                Layout.fillWidth: true
                model: calendarSetDialog.selectedCalendars()
                textRole: "name"
                valueRole: "id"
                currentIndex: Math.max(0, calendarSetDialog.selectionIndex(
                                             calendarSetDialog.defaultCalendarId))
                enabled: model.length > 0
                Accessible.name: qsTr("Default writable calendar for this set")
                onActivated: calendarSetDialog.defaultCalendarId = currentValue
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Save set")
                    primary: true
                    enabled: setName.text.trim().length > 0
                             && calendarSetDialog.selectedIds.length > 0
                    onClicked: {
                        root.upsertCalendarSetRequested({
                            "id": String(calendarSetDialog.setData.id || ""),
                            "name": setName.text.trim(),
                            "calendarIds": calendarSetDialog.selectedIds,
                            "defaultCalendarId": calendarSetDialog.defaultCalendarId
                        })
                        calendarSetDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: credentialDialog
        property var accountData: ({})
        readonly property bool isIcs: accountData.provider === "ics"
        anchors.centerIn: Overlay.overlay
        width: Math.min(430, Overlay.overlay ? Overlay.overlay.width - 48 : 430)
        modal: true
        title: isIcs ? qsTr("Subscription credentials") : qsTr("CalDAV credentials")
        standardButtons: Dialog.Cancel

        function openFor(value) {
            accountData = value || ({})
            credentialUsername.text = String(accountData.principal || "")
            credentialPassword.text = ""
            open()
            credentialUsername.forceActiveFocus()
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingSM
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: credentialDialog.isIcs
                      ? qsTr("Enter both fields to authenticate this feed, or leave both empty to use it without credentials.")
                      : qsTr("The saved server address is retained by the calendar service. Enter the replacement username and app password.")
                color: Theme.mutedText
                font.pixelSize: Theme.smallFontSize
                wrapMode: Text.Wrap
            }
            AppTextField {
                id: credentialUsername
                Layout.fillWidth: true
                placeholderText: credentialDialog.isIcs
                                 ? qsTr("Username (optional)") : qsTr("Username")
                accessibleName: qsTr("Account username")
            }
            AppTextField {
                id: credentialPassword
                Layout.fillWidth: true
                placeholderText: credentialDialog.isIcs
                                 ? qsTr("Password (optional)") : qsTr("Password or app password")
                accessibleName: qsTr("Account password")
                echoMode: TextInput.Password
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Save credentials")
                    primary: true
                    enabled: credentialDialog.isIcs
                             ? (credentialUsername.text.length === 0
                                && credentialPassword.text.length === 0)
                               || (credentialUsername.text.trim().length > 0
                                   && credentialPassword.text.length > 0)
                             : credentialUsername.text.trim().length > 0
                               && credentialPassword.text.length > 0
                    onClicked: {
                        root.updateAccountCredentialsRequested(
                                    String(credentialDialog.accountData.id || ""),
                                    credentialUsername.text.trim(),
                                    credentialPassword.text)
                        credentialPassword.text = ""
                        credentialDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: removeConfirm
        property var accountData: ({})
        anchors.centerIn: Overlay.overlay
        width: Math.min(430, Overlay.overlay ? Overlay.overlay.width - 48 : 430)
        modal: true
        title: qsTr("Remove account?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        function openFor(value) {
            accountData = value
            keepCache.checked = true
            open()
        }
        onAccepted: root.removeAccountRequested(accountData.id, !keepCache.checked)
        contentItem: ColumnLayout {
            spacing: Theme.spacingSM
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: qsTr("Disconnect ") + (removeConfirm.accountData.displayName
                                        || removeConfirm.accountData.principal
                                        || qsTr("this account")) + qsTr("?")
                color: Theme.text
                wrapMode: Text.Wrap
            }
            AppCheckBox {
                id: keepCache
                text: qsTr("Keep downloaded calendar data")
                checked: true
            }
        }
    }

    Dialog {
        id: localCalendarDialog
        anchors.centerIn: Overlay.overlay
        width: Math.min(430, Overlay.overlay ? Overlay.overlay.width - 48 : 430)
        modal: true
        title: qsTr("New local calendar")
        standardButtons: Dialog.Cancel | Dialog.Save
        onAccepted: root.addLocalCalendarRequested(
                        localCalendarName.text.trim(),
                        String(localCalendarColor.selectedColor),
                        localCalendarMuteAlerts.checked)
        contentItem: ColumnLayout {
            spacing: Theme.spacingSM
            AppTextField {
                id: localCalendarName
                Layout.fillWidth: true
                placeholderText: qsTr("Calendar name")
            }
            AppColorPicker {
                id: localCalendarColor
                Layout.fillWidth: true
                selectedColor: "#7aa2f7"
            }
            AppCheckBox {
                id: localCalendarMuteAlerts
                text: qsTr("Mute alerts (reminders & invitations)")
                checked: false
            }
        }
    }

    function accountProvider(accountId) {
        for (let index = 0; index < accounts.length; ++index) {
            if (String(accounts[index].id) === String(accountId))
                return String(accounts[index].provider || "")
        }
        return ""
    }

    function calendarCanBeDeleted(calendar) {
        if (!calendar || !calendar.id || calendar.id === "local-default")
            return false
        const provider = String((calendar.capabilities || {}).provider
                                || accountProvider(calendar.accountId))
        if (provider === "local")
            return true
        return provider === "google"
                && calendar.capabilities.canDeleteCalendar === true
    }
}
