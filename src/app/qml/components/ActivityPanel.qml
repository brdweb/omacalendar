pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

Drawer {
    id: root

    property string mode: "search"
    property var searchResults: []
    property var invitations: []
    property var conflicts: []
    property var operations: []
    property var calendars: []
    property var accounts: []
    property var searchResultsModel: null
    property var invitationsModel: null
    property var conflictsModel: null
    property var operationsModel: null
    readonly property var effectiveSearchResultsModel: searchResultsModel
                                                        || searchResults
    readonly property var effectiveInvitationsModel: invitationsModel
                                                      || invitations
    readonly property var effectiveConflictsModel: conflictsModel || conflicts
    readonly property var effectiveOperationsModel: operationsModel || operations
    property bool connected: false
    property bool syncing: false
    property string statusText: ""
    property string searchText: searchField.text
    property string timeFormat: "system"
    property var pendingInvitation: ({})
    property string pendingInvitationResponse: ""

    signal searchRequested(string query, var filters)
    signal eventActivated(var eventData)
    signal invitationResponseRequested(string invitationId, string recurrenceId,
                                        var expectedLocalRevision, string response,
                                        string recurrenceScope)
    signal invitationSeenRequested(string invitationId)
    signal conflictResolutionRequested(string conflictId, string strategy, var mergedDraft)
    signal conflictMergeRequested(var conflictData)
    signal operationRetryRequested(var operationId)
    signal operationDiscardRequested(var operationId)
    signal syncRequested()

    edge: Qt.RightEdge
    width: Math.min(Theme.panelWidth, Overlay.overlay
                    ? Overlay.overlay.width * 0.42 : Theme.panelWidth)
    height: Overlay.overlay ? Overlay.overlay.height : 720
    modal: false
    dim: false

    onModeChanged: {
        const modes = ["search", "invitations"]
        const nextIndex = modes.indexOf(mode)
        if (nextIndex >= 0)
            tabs.currentIndex = nextIndex
        else
            mode = "search"
    }
    onOpened: {
        if (mode === "search")
            searchField.forceActiveFocus()
    }

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            Layout.bottomMargin: 10

            AppCloseButton {
                toolTipText: "Close activity center"
                onClicked: root.close()
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    text: "Activity center"
                    color: Theme.text
                    font.pixelSize: Theme.fontSize + 4
                    font.weight: Font.Bold
                }
                Text {
                    text: root.connected ? root.statusText : "Calendar service offline"
                    color: Theme.mutedText
                    font.pixelSize: Theme.microFontSize
                    elide: Text.ElideRight
                }
            }
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            currentIndex: 0
            background: Rectangle { color: Theme.darkBackground }
            onCurrentIndexChanged: {
                const modes = ["search", "invitations"]
                root.mode = modes[currentIndex]
                if (currentIndex === 0 && root.opened)
                    searchField.forceActiveFocus()
            }

            AppTabButton { text: "Search" }
            AppTabButton { text: "Invites" + (root.modelCount(
                                                root.effectiveInvitationsModel) > 0
                                            ? "  " + root.modelCount(
                                                root.effectiveInvitationsModel) : "") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    AppTextField {
                        id: searchField
                        Layout.fillWidth: true
                        placeholderText: "Search events, people, notes, locations…"
                        accessibleName: "Search all events"
                        inputMethodHints: Qt.ImhNoPredictiveText
                        onTextEdited: searchDelay.restart()
                        onAccepted: root.submitSearch()

                        Timer {
                            id: searchDelay
                            interval: 180
                            repeat: false
                            onTriggered: root.submitSearch()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        AppComboBox {
                            id: searchCalendarFilter
                            Layout.fillWidth: true
                            model: root.calendarFilterModel()
                            textRole: "name"
                            valueRole: "id"
                            Accessible.name: "Search calendar filter"
                            onActivated: root.submitSearch()
                        }
                        AppComboBox {
                            id: searchAccountFilter
                            Layout.fillWidth: true
                            model: root.accountFilterModel()
                            textRole: "name"
                            valueRole: "id"
                            Accessible.name: "Search account filter"
                            onActivated: root.submitSearch()
                        }
                        AppComboBox {
                            id: searchInvitationFilter
                            Layout.fillWidth: true
                            model: [
                                {"text": "Any response", "value": ""},
                                {"text": "Needs response", "value": "needsAction"},
                                {"text": "Accepted", "value": "accepted"},
                                {"text": "Maybe", "value": "tentative"},
                                {"text": "Declined", "value": "declined"}
                            ]
                            textRole: "text"
                            valueRole: "value"
                            Accessible.name: "Search invitation response filter"
                            onActivated: root.submitSearch()
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        AppTextField {
                            id: searchStartDate
                            Layout.fillWidth: true
                            placeholderText: "From YYYY-MM-DD"
                            accessibleName: "Search start date"
                            onAccepted: root.submitSearch()
                        }
                        AppTextField {
                            id: searchEndDate
                            Layout.fillWidth: true
                            placeholderText: "Through YYYY-MM-DD"
                            accessibleName: "Search end date"
                            onAccepted: root.submitSearch()
                        }
                    }

                    ListView {
                        id: searchList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: root.effectiveSearchResultsModel
                        spacing: 7
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        delegate: EventRow {
                            required property var modelData
                            width: ListView.view.width
                            eventData: modelData
                            showDate: true
                            timeFormat: root.timeFormat
                            onEditRequested: value => root.eventActivated(value)
                        }
                        EmptyState {
                            visible: searchList.count === 0
                            anchors.centerIn: parent
                            width: Math.min(300, parent.width - 24)
                            iconText: "⌕"
                            title: searchField.text.trim().length === 0
                                   ? "Search every calendar"
                                   : "No matching events"
                            description: searchField.text.trim().length === 0
                                         ? "Results include titles, people, notes, and locations."
                                         : "Try fewer words or a different spelling."
                        }
                    }
                }
            }

            Item {
                ListView {
                    id: invitationList
                    anchors.fill: parent
                    anchors.margins: 16
                    model: root.effectiveInvitationsModel
                    spacing: 9
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Rectangle {
                        id: invitationCard
                        required property var modelData
                        width: ListView.view.width
                        implicitHeight: invitationContent.implicitHeight + 24
                        radius: Theme.radius
                        color: Theme.background
                        border.color: Theme.border

                        ColumnLayout {
                            id: invitationContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    text: invitationCard.modelData.summary
                                          || "Untitled invitation"
                                    color: Theme.text
                                    font.pixelSize: Theme.fontSize
                                    font.weight: Font.DemiBold
                                    wrapMode: Text.Wrap
                                }
                                StatusBadge {
                                    visible: invitationCard.modelData.seen !== true
                                    text: "New"
                                    tone: "info"
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.invitationDescription(invitationCard.modelData)
                                color: Theme.mutedText
                                font.pixelSize: Theme.smallFontSize
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                AppButton {
                                    text: "Decline"
                                    compact: true
                                    quiet: true
                                    enabled: root.invitationResponseSupported(
                                                 invitationCard.modelData)
                                    onClicked: root.requestInvitationResponse(
                                                   invitationCard.modelData, "declined")
                                }
                                AppButton {
                                    text: "Maybe"
                                    compact: true
                                    enabled: root.invitationResponseSupported(
                                                 invitationCard.modelData)
                                    onClicked: root.requestInvitationResponse(
                                                   invitationCard.modelData, "tentative")
                                }
                                AppButton {
                                    text: "Accept"
                                    compact: true
                                    primary: true
                                    enabled: root.invitationResponseSupported(
                                                 invitationCard.modelData)
                                    onClicked: root.requestInvitationResponse(
                                                   invitationCard.modelData, "accepted")
                                }
                                Item { Layout.fillWidth: true }
                                AppButton {
                                    visible: invitationCard.modelData.seen !== true
                                    text: "Mark seen"
                                    compact: true
                                    quiet: true
                                    onClicked: root.invitationSeenRequested(
                                                   invitationCard.modelData.id)
                                }
                                AppButton {
                                    text: "Open"
                                    compact: true
                                    quiet: true
                                    onClicked: root.openInvitation(
                                                   invitationCard.modelData)
                                }
                            }
                            Text {
                                visible: !root.invitationResponseSupported(
                                             invitationCard.modelData)
                                Layout.fillWidth: true
                                text: "RSVP is unavailable because this calendar is read-only or its server did not advertise scheduling support."
                                color: Theme.mutedText
                                font.pixelSize: Theme.microFontSize
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    EmptyState {
                        visible: invitationList.count === 0
                        anchors.centerIn: parent
                        width: Math.min(300, parent.width - 24)
                        iconText: "◇"
                        title: "No invitations waiting"
                        description: "New invitations and changed meeting requests appear here."
                    }
                }
            }

            Item {
                ListView {
                    id: conflictList
                    anchors.fill: parent
                    anchors.margins: 16
                    model: root.effectiveConflictsModel
                    spacing: 9
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Rectangle {
                        id: conflictCard
                        required property var modelData
                        width: ListView.view.width
                        implicitHeight: conflictContent.implicitHeight + 24
                        radius: Theme.radius
                        color: Theme.alpha(Theme.danger, 0.075)
                        border.color: Theme.alpha(Theme.danger, 0.28)

                        ColumnLayout {
                            id: conflictContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    text: conflictCard.modelData.summary
                                          || conflictCard.modelData.eventSummary
                                          || "Calendar conflict"
                                    color: Theme.text
                                    font.pixelSize: Theme.fontSize
                                    font.weight: Font.DemiBold
                                    wrapMode: Text.Wrap
                                }
                                StatusBadge { text: "Needs review"; tone: "danger" }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: conflictCard.modelData.message
                                      || "This event changed both locally and remotely. Choose which version to keep."
                                color: Theme.mutedText
                                font.pixelSize: Theme.smallFontSize
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                AppButton {
                                    text: "Keep remote"
                                    compact: true
                                    onClicked: root.conflictResolutionRequested(
                                                   conflictCard.modelData.id,
                                                   "keep_remote", {})
                                }
                                AppButton {
                                    text: "Keep mine"
                                    compact: true
                                    onClicked: root.conflictResolutionRequested(
                                                   conflictCard.modelData.id,
                                                   "keep_local", {})
                                }
                                AppButton {
                                    text: "Review merge"
                                    compact: true
                                    primary: true
                                    onClicked: root.conflictMergeRequested(
                                                   conflictCard.modelData)
                                }
                            }
                        }
                    }

                    EmptyState {
                        visible: conflictList.count === 0
                        anchors.centerIn: parent
                        width: Math.min(300, parent.width - 24)
                        iconText: "✓"
                        title: "No conflicts"
                        description: "When local and remote edits collide, both versions stay safe here."
                    }
                }
            }

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: syncSummary.implicitHeight + 24
                        radius: Theme.radius
                        color: Theme.background
                        border.color: Theme.border
                        RowLayout {
                            id: syncSummary
                            anchors.fill: parent
                            anchors.margins: 12
                            StatusBadge {
                                dotOnly: true
                                text: root.connected ? "Connected" : "Offline"
                                tone: root.connected ? (root.syncing ? "info" : "success")
                                                     : "danger"
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Text {
                                    text: root.syncing ? "Synchronizing"
                                                       : root.connected ? "Calendar service connected"
                                                                        : "Working offline"
                                    color: Theme.text
                                    font.pixelSize: Theme.fontSize
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.statusText
                                    color: Theme.mutedText
                                    font.pixelSize: Theme.smallFontSize
                                    elide: Text.ElideRight
                                }
                            }
                            AppButton {
                                text: "Sync now"
                                compact: true
                                enabled: root.connected && !root.syncing
                                onClicked: root.syncRequested()
                            }
                        }
                    }

                    SectionLabel { text: "PENDING & FAILED OPERATIONS" }
                    ListView {
                        id: operationList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: root.effectiveOperationsModel
                        spacing: 7
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        delegate: Rectangle {
                            id: operationCard
                            required property var modelData
                            width: ListView.view.width
                            implicitHeight: operationRow.implicitHeight + 20
                            radius: Theme.smallRadius
                            color: Theme.background
                            border.color: Theme.border

                            RowLayout {
                                id: operationRow
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 9
                                StatusBadge {
                                    dotOnly: true
                                    text: operationCard.modelData.state || "pending"
                                    tone: operationCard.modelData.state === "blocked"
                                          ? "danger"
                                          : operationCard.modelData.state === "retry_wait"
                                            ? "warning" : "info"
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.operationTitle(operationCard.modelData)
                                        color: Theme.text
                                        font.pixelSize: Theme.smallFontSize
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: operationCard.modelData.errorMessage
                                              || String(operationCard.modelData.state || "pending").replace(/_/g, " ")
                                        color: Theme.mutedText
                                        font.pixelSize: Theme.microFontSize
                                        elide: Text.ElideRight
                                    }
                                }
                                AppButton {
                                    visible: operationCard.modelData.state === "blocked"
                                             || operationCard.modelData.state === "retry_wait"
                                    text: "Retry"
                                    compact: true
                                    onClicked: root.operationRetryRequested(
                                                   operationCard.modelData.id)
                                }
                                AppButton {
                                    text: "Discard"
                                    compact: true
                                    quiet: true
                                    destructive: true
                                    onClicked: root.operationDiscardRequested(
                                                   operationCard.modelData.id)
                                }
                            }
                        }

                        EmptyState {
                            visible: operationList.count === 0
                            anchors.centerIn: parent
                            width: Math.min(300, parent.width - 24)
                            iconText: "✓"
                            title: "Everything is sent"
                            description: "Pending writes, retries, and blocked changes appear here."
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: recurringInvitationDialog
        anchors.centerIn: Overlay.overlay
        width: Math.min(450, Overlay.overlay ? Overlay.overlay.width - 48 : 450)
        modal: true
        title: "Respond to recurring invitation?"
        standardButtons: Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        onOpened: invitationScopeBox.currentIndex = 0

        contentItem: ColumnLayout {
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: "Choose which part of the recurring invitation receives this response."
                color: Theme.text
                font.pixelSize: Theme.smallFontSize
                wrapMode: Text.Wrap
            }
            AppComboBox {
                id: invitationScopeBox
                objectName: "invitationRecurrenceScope"
                Layout.fillWidth: true
                model: root.invitationScopeChoices()
                textRole: "text"
                valueRole: "value"
                Accessible.name: "Invitation recurrence scope"
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    objectName: "confirmInvitationResponse"
                    text: "Send response"
                    primary: true
                    enabled: Boolean(invitationScopeBox.currentValue)
                    onClicked: root.completeInvitationResponse(
                                   invitationScopeBox.currentValue)
                }
            }
        }
    }

    function invitationDescription(value) {
        const when = value.allDay ? value.startDate
                                  : Qt.formatDate(new Date(value.displayStartLocal
                                                           || value.startUtc),
                                                  "ddd, MMM d")
                                    + " · "
                                    + Qt.formatTime(new Date(value.displayStartLocal
                                                             || value.startUtc),
                                                    timePattern())
        const organizer = value.organizer && (value.organizer.displayName
                                              || value.organizer.email)
        return when + (organizer ? "\nFrom " + organizer : "")
    }

    function calendarForId(calendarId) {
        for (let index = 0; index < calendars.length; ++index) {
            if (String(calendars[index].id) === String(calendarId))
                return calendars[index]
        }
        return ({})
    }

    function providerForCalendar(calendarId) {
        const calendar = calendarForId(calendarId)
        for (let index = 0; index < accounts.length; ++index) {
            if (String(accounts[index].id) === String(calendar.accountId || ""))
                return String(accounts[index].provider || "")
        }
        return ""
    }

    function invitationResponseSupported(value) {
        const calendar = calendarForId(value.calendarId)
        if (!calendar.id || calendar.readOnly === true)
            return false
        const capabilities = calendar.capabilities || ({})
        const provider = String(capabilities.provider
                                || providerForCalendar(value.calendarId))
        if (provider === "caldav")
            return capabilities.serverScheduling === true
                   && capabilities.attendeeWrites === true
        return provider === "google" || capabilities.attendeeWrites === true
    }

    function invitationScopeChoices() {
        const choices = [{"text": "Choose recurrence scope…", "value": ""}]
        if (String(pendingInvitation.recurrenceId || "").length > 0)
            choices.push({"text": "This occurrence", "value": "occurrence"})
        choices.push({"text": "Entire series", "value": "series"})
        return choices
    }

    function requestInvitationResponse(value, response) {
        if (!invitationResponseSupported(value))
            return
        if (value.seen !== true)
            invitationSeenRequested(String(value.id || ""))
        if (value.recurrenceRule || value.recurrenceId) {
            pendingInvitation = value
            pendingInvitationResponse = response
            recurringInvitationDialog.open()
            return
        }
        invitationResponseRequested(String(value.id || ""),
                                    String(value.recurrenceId || ""),
                                    Number(value.localRevision), response, "series")
    }

    function completeInvitationResponse(scope) {
        const value = pendingInvitation
        const response = pendingInvitationResponse
        pendingInvitation = ({})
        pendingInvitationResponse = ""
        recurringInvitationDialog.close()
        invitationResponseRequested(String(value.id || ""),
                                    String(value.recurrenceId || ""),
                                    Number(value.localRevision), response,
                                    String(scope))
    }

    function openInvitation(value) {
        if (value.seen !== true)
            invitationSeenRequested(String(value.id || ""))
        eventActivated(value)
    }

    function calendarFilterModel() {
        const values = [{"id": "", "name": "All calendars"}]
        for (let index = 0; index < calendars.length; ++index)
            values.push(calendars[index])
        return values
    }

    function accountFilterModel() {
        const values = [{"id": "", "name": "All accounts"}]
        for (let index = 0; index < accounts.length; ++index) {
            values.push({"id": accounts[index].id,
                         "name": accounts[index].displayName
                                 || accounts[index].principal
                                 || accounts[index].provider || "Account"})
        }
        return values
    }

    function modelCount(value) {
        if (!value)
            return 0
        if (typeof value.count === "number")
            return value.count
        return typeof value.length === "number" ? value.length : 0
    }

    function searchFilters() {
        const result = {}
        if (searchCalendarFilter.currentValue)
            result.calendarIds = [String(searchCalendarFilter.currentValue)]
        if (searchAccountFilter.currentValue)
            result.accountId = String(searchAccountFilter.currentValue)
        if (searchInvitationFilter.currentValue)
            result.invitationState = String(searchInvitationFilter.currentValue)
        const start = new Date(searchStartDate.text.trim() + "T00:00:00")
        if (searchStartDate.text.trim() && !isNaN(start.getTime()))
            result.start = start.toISOString()
        const end = new Date(searchEndDate.text.trim() + "T00:00:00")
        if (searchEndDate.text.trim() && !isNaN(end.getTime())) {
            end.setDate(end.getDate() + 1)
            result.end = end.toISOString()
        }
        return result
    }

    function submitSearch() {
        searchRequested(searchField.text.trim(), searchFilters())
    }

    function operationTitle(value) {
        const operation = String(value.operation || "change")
        return operation.charAt(0).toUpperCase() + operation.slice(1)
                + (value.eventSummary ? " · " + value.eventSummary : "")
    }

    function timePattern() {
        if (timeFormat === "24h")
            return "HH:mm"
        if (timeFormat === "12h")
            return "h:mm AP"
        return Qt.locale().timeFormat(Locale.ShortFormat)
    }
}
