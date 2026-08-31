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
    property var preferences: ({})
    property string systemTimeZoneId: "UTC"
    property var availableTimeZoneIds: ["UTC"]
    property bool bundledGoogleOAuthAvailable: false
    property bool googleOAuthConfigured: false
    readonly property var timeZoneOptions: buildTimeZoneOptions()
    readonly property var defaultCalendarOptions: buildDefaultCalendarOptions()

    signal connectGoogleRequested(string displayName)
    signal connectGoogleClientRequested(string clientId, string displayName)
    signal connectGoogleCredentialsRequested(string displayName)
    signal addCalDavRequested(string endpoint, string username,
                              string password, string displayName)
    signal addLocalCalendarRequested(string name, string color)
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

    function buildTimeZoneOptions() {
        const result = [{"text": "System default — " + systemTimeZoneId,
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
                result.push({"text": String(calendar.name || "Calendar"),
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

        for (let index = 0; index < ordered.length; ++index) {
            if (Number(ordered[index].position || 0) !== index)
                calendarPreferenceChanged(String(ordered[index].id),
                                          "position", index)
        }
    }

    edge: Qt.RightEdge
    width: Math.min(560, Overlay.overlay ? Overlay.overlay.width * 0.52 : 560)
    height: Overlay.overlay ? Overlay.overlay.height : 760
    modal: true

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            spacing: 10
            AppCloseButton {
                toolTipText: "Close settings"
                onClicked: root.close()
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    text: "Accounts & settings"
                    color: Theme.text
                    font.pixelSize: Theme.titleFontSize
                    font.weight: Font.Bold
                }
                Text {
                    text: "Credentials stay in your desktop keyring."
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                }
            }
        }

        TabBar {
            id: settingsTabs
            Layout.fillWidth: true
            background: Rectangle { color: Theme.darkBackground }
            AppTabButton { text: "Accounts" }
            AppTabButton { text: "Calendars" }
            AppTabButton { text: "Preferences" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: settingsTabs.currentIndex

            ScrollView {
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded
                ColumnLayout {
                    width: root.width - 40
                    x: 20
                    spacing: 13

                    Item { Layout.preferredHeight: 5 }
                    AppAccordionSection {
                        title: "Connected accounts"
                        detail: String(root.accounts.length)
                        expanded: true

                        Repeater {
                        model: root.effectiveAccountsModel
                        delegate: Rectangle {
                            id: accountCard
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: accountRow.implicitHeight + 24
                            radius: Theme.radius
                            color: Theme.background
                            border.color: Theme.border

                            RowLayout {
                                id: accountRow
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 10
                                StatusBadge {
                                    dotOnly: true
                                    text: accountCard.modelData.authStatus || "unknown"
                                    tone: accountCard.modelData.authStatus === "connected"
                                          ? "success"
                                          : accountCard.modelData.authStatus === "reauthorization_required"
                                            ? "danger" : "warning"
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        Layout.fillWidth: true
                                        text: accountCard.modelData.displayName
                                              || accountCard.modelData.principal
                                              || "Calendar account"
                                        color: Theme.text
                                        font.pixelSize: Theme.fontSize
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: String(accountCard.modelData.provider || "calendar").toUpperCase()
                                              + " · "
                                              + String(accountCard.modelData.authStatus || "connected").replace(/_/g, " ")
                                        color: Theme.mutedText
                                        font.pixelSize: Theme.microFontSize
                                        elide: Text.ElideRight
                                    }
                                }
                                AppButton {
                                    visible: accountCard.modelData.provider !== "local"
                                    text: accountCard.modelData.provider === "ics"
                                          ? "Refresh" : "Sync"
                                    compact: true
                                    quiet: true
                                    enabled: root.connected && !root.busy
                                    onClicked: root.syncAccountRequested(
                                                   String(accountCard.modelData.id))
                                }
                                AppButton {
                                    visible: accountCard.modelData.provider === "google"
                                             && (accountCard.modelData.authStatus
                                                 === "reauthorization_required"
                                                 || accountCard.modelData.authStatus
                                                    === "disconnected")
                                    text: "Reauthorize"
                                    compact: true
                                    primary: true
                                    onClicked: root.reauthorizeAccountRequested(
                                                   String(accountCard.modelData.id))
                                }
                                AppButton {
                                    visible: accountCard.modelData.provider === "caldav"
                                             || accountCard.modelData.provider === "ics"
                                    text: accountCard.modelData.authStatus
                                          === "reauthorization_required"
                                          || accountCard.modelData.authStatus
                                             === "disconnected"
                                          ? "Reconnect" : "Credentials"
                                    compact: true
                                    primary: accountCard.modelData.authStatus
                                             === "reauthorization_required"
                                             || accountCard.modelData.authStatus
                                                === "disconnected"
                                    quiet: !primary
                                    onClicked: credentialDialog.openFor(
                                                   accountCard.modelData)
                                }
                                AppButton {
                                    visible: accountCard.modelData.provider !== "local"
                                    text: "Remove"
                                    quiet: true
                                    compact: true
                                    destructive: true
                                    onClicked: removeConfirm.openFor(accountCard.modelData)
                                }
                            }
                        }
                        }

                        EmptyState {
                            visible: root.accounts.length === 0
                            Layout.fillWidth: true
                            Layout.preferredHeight: 145
                            iconText: "◌"
                            title: "No connected accounts"
                            description: "Local calendars still work without a network account."
                        }
                    }

                    AppAccordionSection {
                        title: "Google Calendar"
                        expanded: true

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: googleContent.implicitHeight + 28
                            radius: Theme.radius
                            color: Theme.background
                            border.color: Theme.border
                            ColumnLayout {
                            id: googleContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 14
                            spacing: 9
                            Text {
                                Layout.fillWidth: true
                                text: "Authorization opens in your browser and returns through a secure local callback. OmaCalendar requests calendar and event access only."
                                color: Theme.mutedText
                                font.pixelSize: Theme.smallFontSize
                                wrapMode: Text.Wrap
                            }
                            AppTextField {
                                id: googleName
                                Layout.fillWidth: true
                                placeholderText: "Account label (optional)"
                            }
                            AppTextField {
                                id: googleClientId
                                visible: !root.bundledGoogleOAuthAvailable
                                         && !root.googleOAuthConfigured
                                Layout.fillWidth: true
                                placeholderText: "Desktop OAuth client ID"
                                accessibleName: "Google Desktop OAuth client ID"
                                inputMethodHints: Qt.ImhNoAutoUppercase
                            }
                            AppButton {
                                Layout.fillWidth: true
                                text: "Continue with Google in browser"
                                primary: true
                                enabled: root.connected && !root.busy
                                         && (root.bundledGoogleOAuthAvailable
                                             || root.googleOAuthConfigured
                                             || googleClientId.text.trim().length > 0)
                                onClicked: {
                                    if (root.bundledGoogleOAuthAvailable)
                                        root.connectGoogleRequested(
                                                    googleName.text.trim())
                                    else if (root.googleOAuthConfigured)
                                        root.connectGoogleRequested(
                                                    googleName.text.trim())
                                    else
                                        root.connectGoogleClientRequested(
                                                    googleClientId.text.trim(),
                                                    googleName.text.trim())
                                }
                            }
                            }
                        }
                    }

                    AppAccordionSection {
                        title: "CalDAV"
                        expanded: false

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: caldavContent.implicitHeight + 28
                            radius: Theme.radius
                            color: Theme.background
                            border.color: Theme.border
                            ColumnLayout {
                            id: caldavContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 14
                            spacing: 9
                            AppTextField {
                                id: caldavName
                                Layout.fillWidth: true
                                placeholderText: "Account label (optional)"
                            }
                            AppTextField {
                                id: caldavEndpoint
                                Layout.fillWidth: true
                                placeholderText: "https://caldav.example.com/"
                                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                            }
                            AppTextField {
                                id: caldavUser
                                Layout.fillWidth: true
                                placeholderText: "Username"
                                inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                            }
                            AppTextField {
                                id: caldavPassword
                                Layout.fillWidth: true
                                placeholderText: "Password or app password"
                                echoMode: TextInput.Password
                            }
                            AppButton {
                                Layout.fillWidth: true
                                text: "Connect CalDAV"
                                primary: true
                                enabled: root.connected && !root.busy
                                         && caldavEndpoint.text.trim().length > 0
                                         && caldavUser.text.trim().length > 0
                                         && caldavPassword.text.length > 0
                                onClicked: {
                                    root.addCalDavRequested(caldavEndpoint.text.trim(),
                                                           caldavUser.text.trim(),
                                                           caldavPassword.text,
                                                           caldavName.text.trim())
                                    caldavPassword.text = ""
                                }
                            }
                            }
                        }
                    }

                    AppAccordionSection {
                        title: "ICS subscription"
                        expanded: false

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 196
                            radius: Theme.radius
                            color: Theme.background
                            border.color: Theme.border
                            ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 8
                            AppTextField {
                                id: icsName
                                Layout.fillWidth: true
                                placeholderText: "Subscription name (optional)"
                            }
                            AppTextField {
                                id: icsUrl
                                Layout.fillWidth: true
                                placeholderText: "https://example.com/calendar.ics"
                                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                AppTextField {
                                    id: icsUser
                                    Layout.fillWidth: true
                                    placeholderText: "Username (optional)"
                                }
                                AppTextField {
                                    id: icsPassword
                                    Layout.fillWidth: true
                                    placeholderText: "Password (optional)"
                                    echoMode: TextInput.Password
                                }
                            }
                            AppButton {
                                Layout.fillWidth: true
                                text: "Add read-only subscription"
                                enabled: root.connected && icsUrl.text.trim().length > 0
                                onClicked: {
                                    root.addIcsSubscriptionRequested(icsUrl.text.trim(),
                                                                     icsUser.text.trim(),
                                                                     icsPassword.text,
                                                                     icsName.text.trim())
                                    icsPassword.text = ""
                                }
                            }
                            }
                        }
                    }
                    Item { Layout.preferredHeight: 12 }
                }
            }

            ScrollView {
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: root.width - 40
                    x: 20
                    spacing: 11
                    Item { Layout.preferredHeight: 5 }

                    RowLayout {
                        Layout.fillWidth: true
                        SectionLabel { Layout.fillWidth: true; text: "CALENDAR SETS" }
                        AppButton {
                            iconText: "+"
                            text: "New set"
                            compact: true
                            onClicked: calendarSetDialog.openNew()
                        }
                    }

                    Repeater {
                        model: root.effectiveCalendarSetsModel
                        delegate: Rectangle {
                            id: setCard
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: setRow.implicitHeight + 20
                            radius: Theme.radius
                            color: Theme.background
                            border.color: Theme.border
                            RowLayout {
                                id: setRow
                                anchors.fill: parent
                                anchors.margins: 10
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        Layout.fillWidth: true
                                        text: setCard.modelData.name || "Calendar set"
                                        color: Theme.text
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: (setCard.modelData.calendarIds || []).length
                                              + " calendar(s)"
                                        color: Theme.mutedText
                                        font.pixelSize: Theme.microFontSize
                                    }
                                }
                                StatusBadge {
                                    visible: setCard.modelData.isDefault === true
                                    text: "Built in"
                                    tone: "neutral"
                                }
                                AppButton {
                                    visible: setCard.modelData.isDefault !== true
                                    text: "Edit"
                                    compact: true
                                    quiet: true
                                    onClicked: calendarSetDialog.openExisting(
                                                   setCard.modelData)
                                }
                                AppButton {
                                    visible: setCard.modelData.isDefault !== true
                                    iconText: "×"
                                    compact: true
                                    quiet: true
                                    destructive: true
                                    toolTipText: "Remove calendar set"
                                    onClicked: root.removeCalendarSetRequested(
                                                   String(setCard.modelData.id))
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        SectionLabel { Layout.fillWidth: true; text: "CALENDARS" }
                        AppButton {
                            iconText: "+"
                            text: "New local"
                            compact: true
                            onClicked: localCalendarDialog.open()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: defaultCalendarRow.implicitHeight + 28
                        radius: Theme.radius
                        color: Theme.surfaceAlt
                        border.color: Theme.border

                        RowLayout {
                            id: defaultCalendarRow
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 18

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                Text {
                                    Layout.fillWidth: true
                                    text: "Default calendar"
                                    color: Theme.text
                                    font.pixelSize: Theme.fontSize
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: "Used automatically for new events in the app and widget."
                                    color: Theme.mutedText
                                    font.pixelSize: Theme.smallFontSize
                                    wrapMode: Text.Wrap
                                }
                            }

                            AppComboBox {
                                id: defaultCalendarSelector
                                objectName: "defaultCalendarSelector"
                                Layout.preferredWidth: 220
                                model: root.defaultCalendarOptions
                                textRole: "text"
                                valueRole: "value"
                                currentIndex: root.defaultCalendarIndex()
                                enabled: count > 0
                                Accessible.name: "Default calendar for new events"
                                onActivated: index => {
                                    const option = root.defaultCalendarOptions[index]
                                    if (option)
                                        root.preferenceChanged("defaultCalendarId",
                                                               option.value)
                                }
                            }
                        }
                    }

                    Repeater {
                        model: root.effectiveCalendarsModel
                        delegate: Rectangle {
                            id: calendarCard
                            required property var modelData
                            objectName: "calendarCard-" + String(modelData.id || "")
                            readonly property string calendarId: String(
                                                                      modelData.id
                                                                      || "")
                            Layout.fillWidth: true
                            implicitHeight: calendarSettings.implicitHeight + 24
                            radius: Theme.radius
                            z: reorderMouse.drag.active ? 100
                               : calendarDropArea.validDrop ? 50 : 0
                            color: calendarDropArea.validDrop
                                   ? Theme.alpha(Theme.accent, 0.12)
                                   : Theme.background
                            border.width: calendarDropArea.validDrop ? 2 : 1
                            border.color: calendarDropArea.validDrop
                                          ? Theme.accent : Theme.border
                            opacity: reorderMouse.drag.active ? 0.68 : 1

                            DropArea {
                                id: calendarDropArea
                                objectName: "calendarDropArea-"
                                            + calendarCard.calendarId
                                anchors.fill: parent
                                property bool placeAfter: false
                                function sourceCalendarId(source) {
                                    if (!source)
                                        return ""
                                    const prefix = "calendarCard-"
                                    const sourceName = String(source.objectName || "")
                                    return sourceName.startsWith(prefix)
                                            ? sourceName.slice(prefix.length) : ""
                                }
                                readonly property string draggedCalendarId:
                                    sourceCalendarId(drag.source)
                                readonly property bool validDrop:
                                    containsDrag && draggedCalendarId
                                    && draggedCalendarId !== calendarCard.calendarId
                                onEntered: drag => placeAfter = drag.y > height / 2
                                onPositionChanged: drag =>
                                                       placeAfter = drag.y > height / 2
                                onDropped: drop => {
                                    const sourceId = sourceCalendarId(drop.source)
                                    if (sourceId)
                                        root.reorderCalendar(sourceId,
                                                    calendarCard.calendarId,
                                                    placeAfter)
                                    drop.acceptProposedAction()
                                }
                            }

                            Rectangle {
                                z: 12
                                objectName: "calendarDropIndicator-"
                                            + calendarCard.calendarId
                                visible: calendarDropArea.validDrop
                                x: 6
                                y: calendarDropArea.placeAfter
                                   ? calendarCard.height - height / 2 : -height / 2
                                width: calendarCard.width - 12
                                height: 28
                                radius: 8
                                color: Theme.accent

                                Text {
                                    anchors.centerIn: parent
                                    width: parent.width - 20
                                    text: "Drop "
                                          + (calendarDropArea.placeAfter
                                             ? "after " : "before ")
                                          + String(calendarCard.modelData.name
                                                   || "calendar")
                                    color: Theme.accentText
                                    font.pixelSize: Theme.smallFontSize
                                    font.weight: Font.DemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                }
                            }

                            ColumnLayout {
                                id: calendarSettings
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 6
                                RowLayout {
                                    Layout.fillWidth: true

                                    Item {
                                        id: dragHandle
                                        objectName: "calendarDragHandle-"
                                                    + calendarCard.calendarId
                                        Layout.preferredWidth: 30
                                        Layout.preferredHeight: 30
                                        Accessible.name: "Drag to reorder "
                                                         + String(calendarCard.modelData.name
                                                                  || "calendar")
                                        Accessible.role: Accessible.Button

                                        Text {
                                            z: 1
                                            anchors.centerIn: parent
                                            text: "≡"
                                            color: reorderMouse.drag.active
                                                   ? Theme.accent
                                                   : reorderMouse.containsMouse
                                                   ? Theme.text : Theme.mutedText
                                            font.pixelSize: Theme.fontSize + 5
                                            font.weight: Font.DemiBold
                                        }
                                        MouseArea {
                                            id: reorderMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: pressed
                                                         ? Qt.ClosedHandCursor
                                                         : Qt.OpenHandCursor
                                            drag.target: dragProxy
                                            onReleased: {
                                                dragProxy.Drag.drop()
                                                dragProxy.x = 0
                                                dragProxy.y = 0
                                            }
                                        }
                                        Item {
                                            id: dragProxy
                                            objectName: "calendarDragPreview-"
                                                        + calendarCard.calendarId
                                            z: 100
                                            width: 210
                                            height: 38
                                            visible: reorderMouse.drag.active
                                            Drag.active: reorderMouse.drag.active
                                            Drag.source: calendarCard
                                            Drag.supportedActions: Qt.MoveAction
                                            Drag.hotSpot.x: 18
                                            Drag.hotSpot.y: height / 2

                                            Rectangle {
                                                anchors.fill: parent
                                                radius: 9
                                                color: Theme.surface
                                                border.width: 2
                                                border.color: Theme.accent

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 10
                                                    anchors.rightMargin: 12
                                                    spacing: 8
                                                    Rectangle {
                                                        Layout.preferredWidth: 10
                                                        Layout.preferredHeight: 10
                                                        radius: 5
                                                        color: calendarCard.modelData.colorOverride
                                                               || calendarCard.modelData.color
                                                               || Theme.accent
                                                    }
                                                    Text {
                                                        Layout.fillWidth: true
                                                        text: calendarCard.modelData.name
                                                              || "Calendar"
                                                        color: Theme.text
                                                        font.pixelSize: Theme.smallFontSize
                                                        font.weight: Font.DemiBold
                                                        elide: Text.ElideRight
                                                    }
                                                    Text {
                                                        text: "Move"
                                                        color: Theme.accent
                                                        font.pixelSize: Theme.microFontSize
                                                        font.weight: Font.DemiBold
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    Rectangle {
                                        Layout.preferredWidth: 10
                                        Layout.preferredHeight: 10
                                        radius: 5
                                        color: calendarCard.modelData.colorOverride
                                               || calendarCard.modelData.color
                                               || Theme.accent
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: calendarCard.modelData.name || "Calendar"
                                        color: Theme.text
                                        font.pixelSize: Theme.fontSize
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    StatusBadge {
                                        visible: calendarCard.modelData.readOnly === true
                                        text: "Read only"
                                        tone: "neutral"
                                    }
                                    AppButton {
                                        objectName: "deleteLocalCalendar-"
                                                    + String(calendarCard.modelData.id || "")
                                        visible: root.calendarCanBeDeleted(
                                                     calendarCard.modelData)
                                        text: "Delete"
                                        compact: true
                                        quiet: true
                                        destructive: true
                                        toolTipText: "Permanently delete calendar and its events"
                                        onClicked: localCalendarRemoveConfirm.openFor(
                                                       calendarCard.modelData)
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 14
                                    AppCheckBox {
                                        text: "Visible"
                                        checked: calendarCard.modelData.visible !== false
                                                 && calendarCard.modelData.enabled !== false
                                        onToggled: root.calendarPreferenceChanged(
                                                       calendarCard.modelData.id,
                                                       "visible", checked)
                                    }
                                    AppCheckBox {
                                        id: muteInvitationAlerts
                                        objectName: "muteInvitationAlerts-"
                                                    + calendarCard.calendarId
                                        text: "Mute invitation alerts"
                                        checked: calendarCard.modelData.ignoreAlerts === true
                                        ToolTip.visible: hovered
                                                             && !reorderMouse.drag.active
                                        ToolTip.delay: 450
                                        ToolTip.text: "Suppresses desktop notifications for new, changed, or cancelled invitations on this calendar. Event reminders still fire."
                                        Accessible.description: ToolTip.text
                                        onToggled: root.calendarPreferenceChanged(
                                                       calendarCard.modelData.id,
                                                       "ignoreAlerts", checked)
                                    }
                                    Item { Layout.fillWidth: true }
                                    AppColorPicker {
                                        objectName: "calendarColorPicker-"
                                                    + calendarCard.calendarId
                                        Layout.preferredWidth: 132
                                        selectedColor: calendarCard.modelData.colorOverride
                                                       || calendarCard.modelData.color
                                                       || Theme.accent
                                        allowClear: true
                                        onColorSelected: colorValue =>
                                                             root.calendarPreferenceChanged(
                                                                 calendarCard.modelData.id,
                                                                 "colorOverride",
                                                                 colorValue)
                                    }
                                }
                            }
                        }
                    }
                    Item { Layout.preferredHeight: 12 }
                }
            }

            ScrollView {
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: root.width - 40
                    x: 20
                    spacing: 13
                    Item { Layout.preferredHeight: 5 }

                    SectionLabel { text: "DISPLAY" }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: displayPreferences.implicitHeight + 24
                        radius: Theme.radius
                        color: Theme.background
                        border.color: Theme.border
                        ColumnLayout {
                            id: displayPreferences
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 10
                            AppComboBox {
                                Layout.fillWidth: true
                                model: ["System time format", "12-hour", "24-hour"]
                                currentIndex: Math.max(0, ["system", "12h", "24h"].indexOf(
                                                           String(root.preferences.timeFormat
                                                                  || "system")))
                                Accessible.name: "Time format"
                                onActivated: index => root.preferenceChanged(
                                                 "timeFormat", ["system", "12h", "24h"][index])
                            }
                            AppComboBox {
                                Layout.fillWidth: true
                                model: ["System week start", "Monday", "Sunday"]
                                currentIndex: Math.max(0, [0, 1, 7].indexOf(
                                                           Number(root.preferences.firstDayOfWeek
                                                                  || 0)))
                                Accessible.name: "First day of week"
                                onActivated: index => root.preferenceChanged(
                                                 "firstDayOfWeek", [0, 1, 7][index])
                            }
                            AppComboBox {
                                id: displayTimeZoneBox
                                objectName: "displayTimeZone"
                                Layout.fillWidth: true
                                model: root.timeZoneOptions
                                textRole: "text"
                                valueRole: "value"
                                currentIndex: root.displayTimeZoneIndex()
                                Accessible.name: "Display time zone"
                                onActivated: root.preferenceChanged("displayTimeZone",
                                                                    currentValue)
                            }
                            AppComboBox {
                                Layout.fillWidth: true
                                model: [
                                    {"text": "Default duration: 15 minutes", "value": 15},
                                    {"text": "Default duration: 30 minutes", "value": 30},
                                    {"text": "Default duration: 45 minutes", "value": 45},
                                    {"text": "Default duration: 1 hour", "value": 60},
                                    {"text": "Default duration: 90 minutes", "value": 90},
                                    {"text": "Default duration: 2 hours", "value": 120}
                                ]
                                textRole: "text"
                                valueRole: "value"
                                currentIndex: Math.max(0, [15, 30, 45, 60, 90, 120].indexOf(
                                                           Number(root.preferences.defaultDuration
                                                                  || 60)))
                                Accessible.name: "Default event duration"
                                onActivated: root.preferenceChanged("defaultDuration",
                                                                    currentValue)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    text: "Work hours"
                                    color: Theme.mutedText
                                    font.pixelSize: Theme.smallFontSize
                                }
                                AppSpinBox {
                                    from: 0
                                    to: 23
                                    value: Number(root.preferences.workDayStart || 8)
                                    Accessible.name: "Work day start hour"
                                    onValueModified: root.preferenceChanged("workDayStart",
                                                                             value)
                                }
                                Text { text: "to"; color: Theme.mutedText }
                                AppSpinBox {
                                    from: 1
                                    to: 24
                                    value: Number(root.preferences.workDayEnd || 18)
                                    Accessible.name: "Work day end hour"
                                    onValueModified: root.preferenceChanged("workDayEnd",
                                                                             value)
                                }
                            }
                        }
                    }

                    SectionLabel { text: "IMPORT & EXPORT" }
                    RowLayout {
                        Layout.fillWidth: true
                        AppButton {
                            Layout.fillWidth: true
                            text: "Import .ics…"
                            onClicked: root.importIcsRequested()
                        }
                        AppButton {
                            Layout.fillWidth: true
                            text: "Export .ics…"
                            onClicked: root.exportIcsRequested()
                        }
                    }
                    SectionLabel { text: "SUPPORT" }
                    AppButton {
                        Layout.fillWidth: true
                        text: "Preview diagnostics"
                        onClicked: root.diagnosticsRequested()
                    }
                    Item { Layout.preferredHeight: 12 }
                }
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
        title: "Delete calendar?"
        standardButtons: Dialog.Cancel | Dialog.Ok
        function openFor(value) {
            calendarData = value || ({})
            open()
        }
        onAccepted: root.removeCalendarRequested(
                        String(calendarData.id || ""))
        contentItem: Text {
            width: 380
            text: {
                const value = localCalendarRemoveConfirm.calendarData
                const provider = String((value.capabilities || {}).provider
                                        || root.accountProvider(value.accountId))
                const location = provider === "google"
                        ? " from Google Calendar" : " from this device"
                return "Permanently delete " + (value.name || "this calendar")
                        + location + " and all of its events? This cannot be undone."
            }
            color: Theme.text
            wrapMode: Text.Wrap
        }
    }

    Dialog {
        id: calendarSetDialog
        property var setData: ({})
        property var selectedIds: []
        property string defaultCalendarId: ""
        property int selectionRevision: 0
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 48 : 460)
        modal: true
        title: setData.id ? "Edit calendar set" : "New calendar set"
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
            spacing: 10
            AppTextField {
                id: setName
                Layout.fillWidth: true
                placeholderText: "Set name"
                accessibleName: "Calendar set name"
            }
            Text {
                Layout.fillWidth: true
                text: "Select calendars. Use the arrows to control their order."
                color: Theme.mutedText
                font.pixelSize: Theme.smallFontSize
                wrapMode: Text.Wrap
            }
            ColumnLayout {
                Layout.fillWidth: true
                Repeater {
                    model: root.effectiveCalendarsModel
                    delegate: RowLayout {
                        id: membershipRow
                        required property var modelData
                        Layout.fillWidth: true
                        readonly property int selectedIndex:
                            calendarSetDialog.selectionIndex(modelData.id)
                        AppCheckBox {
                            Layout.fillWidth: true
                            text: membershipRow.modelData.name || "Calendar"
                            checked: membershipRow.selectedIndex >= 0
                            onToggled: calendarSetDialog.toggleCalendar(
                                           membershipRow.modelData.id, checked)
                        }
                        AppButton {
                            visible: membershipRow.selectedIndex >= 0
                            iconText: "↑"
                            compact: true
                            quiet: true
                            enabled: membershipRow.selectedIndex > 0
                            toolTipText: "Move earlier"
                            onClicked: calendarSetDialog.moveCalendar(
                                           membershipRow.modelData.id, -1)
                        }
                        AppButton {
                            visible: membershipRow.selectedIndex >= 0
                            iconText: "↓"
                            compact: true
                            quiet: true
                            enabled: membershipRow.selectedIndex
                                     < calendarSetDialog.selectedIds.length - 1
                            toolTipText: "Move later"
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
                Accessible.name: "Default writable calendar for this set"
                onActivated: calendarSetDialog.defaultCalendarId = currentValue
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: "Save set"
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
        title: isIcs ? "Subscription credentials" : "CalDAV credentials"
        standardButtons: Dialog.Cancel

        function openFor(value) {
            accountData = value || ({})
            credentialUsername.text = String(accountData.principal || "")
            credentialPassword.text = ""
            open()
            credentialUsername.forceActiveFocus()
        }

        contentItem: ColumnLayout {
            spacing: 10
            Text {
                Layout.fillWidth: true
                text: credentialDialog.isIcs
                      ? "Enter both fields to authenticate this feed, or leave both empty to use it without credentials."
                      : "The saved server address is retained by the calendar service. Enter the replacement username and app password."
                color: Theme.mutedText
                font.pixelSize: Theme.smallFontSize
                wrapMode: Text.Wrap
            }
            AppTextField {
                id: credentialUsername
                Layout.fillWidth: true
                placeholderText: credentialDialog.isIcs
                                 ? "Username (optional)" : "Username"
                accessibleName: "Account username"
            }
            AppTextField {
                id: credentialPassword
                Layout.fillWidth: true
                placeholderText: credentialDialog.isIcs
                                 ? "Password (optional)" : "Password or app password"
                accessibleName: "Account password"
                echoMode: TextInput.Password
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: "Save credentials"
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
        title: "Remove account?"
        standardButtons: Dialog.Cancel | Dialog.Ok
        function openFor(value) {
            accountData = value
            keepCache.checked = true
            open()
        }
        onAccepted: root.removeAccountRequested(accountData.id, !keepCache.checked)
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                Layout.fillWidth: true
                text: "Disconnect " + (removeConfirm.accountData.displayName
                                        || removeConfirm.accountData.principal
                                        || "this account") + "?"
                color: Theme.text
                wrapMode: Text.Wrap
            }
            AppCheckBox {
                id: keepCache
                text: "Keep downloaded calendar data"
                checked: true
            }
        }
    }

    Dialog {
        id: localCalendarDialog
        anchors.centerIn: Overlay.overlay
        width: Math.min(430, Overlay.overlay ? Overlay.overlay.width - 48 : 430)
        modal: true
        title: "New local calendar"
        standardButtons: Dialog.Cancel | Dialog.Save
        onAccepted: root.addLocalCalendarRequested(
                        localCalendarName.text.trim(),
                        String(localCalendarColor.selectedColor))
        contentItem: ColumnLayout {
            spacing: 9
            AppTextField {
                id: localCalendarName
                Layout.fillWidth: true
                placeholderText: "Calendar name"
            }
            AppColorPicker {
                id: localCalendarColor
                Layout.fillWidth: true
                selectedColor: "#7aa2f7"
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
