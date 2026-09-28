pragma ComponentBehavior: Bound
// App and OmarchyTheme are intentionally supplied as context properties.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import OmaCalendar
import "components"
import "views"
import "CalendarVisibility.js" as CalendarVisibility
import "DateRange.js" as DateRange
import "EventIndex.js" as EventIndex

ApplicationWindow {
    id: window

    width: 1440
    height: 900
    minimumWidth: 980
    minimumHeight: 560
    visible: true
    title: "OmaCalendar"
    color: Theme.background
    font.pixelSize: Theme.fontSize

    // Basic-style dialogs and standard buttons inherit this palette. Keep
    // their surfaces and labels in the same theme as custom content.
    palette.window: Theme.surface
    palette.windowText: Theme.text
    palette.base: Theme.background
    palette.alternateBase: Theme.surfaceAlt
    palette.text: Theme.text
    palette.button: Theme.surfaceAlt
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentText
    palette.placeholderText: Theme.mutedText
    palette.dark: Theme.border
    palette.mid: Theme.border
    palette.light: Theme.surfaceAlt
    palette.shadow: Theme.darkBackground

    property string currentView: "month"
    property date visibleMonth: new Date(App.selectedDate.getFullYear(),
                                         App.selectedDate.getMonth(), 1)
    property var calendarVisibilityOverrides: ({})
    readonly property string activeCalendarSetId: String(
                                                     appValue("activeCalendarSetId",
                                                              "all-calendars"))
    property var localSearchResults: []
    property var activeSearchFilters: ({})
    property var selectedEvent: ({})
    readonly property string selectedEventReference: eventReference(selectedEvent)
    property bool sidebarVisible: true
    // Set when the sidebar was hidden because the window became narrow, so the
    // manual toggle keeps working and the sidebar returns when there is room.
    property bool sidebarAutoHidden: false
    property bool firstLoadComplete: false
    // Bumped every minute so relative sync labels stay current.
    property int clockTick: 0
    property var pendingMoveEvent: ({})
    property var pendingMoveOptions: ({})
    property var pendingExportScope: ({})
    property string pendingGoogleDisplayName: ""
    // Timeline scroll positions survive switching away from a view; negative
    // lets the view choose (the current time today, else the work day).
    property real dayScrollY: -1
    property real weekScrollY: -1
    // Days the agenda lists; it grows as the user scrolls past the end.
    property int agendaDayCount: 31

    readonly property var decoratedEvents: decorateEvents(App.events)
    readonly property var visibleEvents: filterVisibleEvents(decoratedEvents)
    readonly property var calendarSets: appList("calendarSets")
    readonly property var sidebarCalendars: CalendarVisibility.calendarsForSidebar(
                                                appList("calendars"), calendarSets,
                                                activeCalendarSetId,
                                                calendarVisibilityOverrides)
    readonly property var writableCalendars: appList("calendars").filter(
                                                 function(calendar) {
                                                     return calendar.enabled !== false
                                                             && !calendar.readOnly
                                                 })
    readonly property var localWritableCalendars: writableCalendars.filter(
                                                      function(calendar) {
                                                          return window.accountProvider(
                                                                      calendar.accountId)
                                                                  === "local"
                                                      })
    readonly property var invitations: appList("invitations")
    readonly property var conflicts: appList("conflicts")
    readonly property var operations: appList("operations")
    readonly property var searchResults: App.connected
                                         ? appList("searchResults")
                                         : localSearchResults
    readonly property var preferences: appObject("preferences")
    readonly property int configuredFirstDayOfWeek:
        preferences.firstDayOfWeek === undefined
        ? 0 : Number(preferences.firstDayOfWeek)
    readonly property int firstDayOfWeek:
        configuredFirstDayOfWeek === 0
        ? Number(Qt.locale().firstDayOfWeek) : configuredFirstDayOfWeek
    // Stored settings round-trip as JSON, so accept a boolean or its text.
    readonly property bool showWeekNumbers: preferences.showWeekNumbers === true
                                            || preferences.showWeekNumbers === "true"
    // Recomputed per selected date and on any preference change (including
    // the display zone), so offsets follow DST in both zones.
    readonly property var secondaryTime: {
        const zone = String(preferences.secondaryTimeZone || "")
        if (zone.length === 0 || typeof App.secondaryTimeLabels !== "function")
            return ({})
        return App.secondaryTimeLabels(Qt.formatDate(App.selectedDate, "yyyy-MM-dd"), zone)
    }
    readonly property int currentViewIndex: viewIndex(currentView)
    readonly property var latestSyncDate: latestCalendarSync()

    onWidthChanged: updateSidebarForWidth()

    Timer {
        interval: 60000
        running: true
        repeat: true
        onTriggered: window.clockTick = window.clockTick + 1
    }

    header: Rectangle {
        implicitHeight: 68
        color: Theme.darkBackground
        border.color: Theme.divider

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingLG
            anchors.rightMargin: Theme.spacingLG
            spacing: Theme.spacingSM

            AppButton {
                iconText: window.sidebarVisible ? Theme.glyphSidebarShown
                                                : Theme.glyphSidebarHidden
                quiet: true
                compact: true
                toolTipText: window.sidebarVisible ? qsTr("Hide sidebar") : qsTr("Show sidebar")
                onClicked: window.toggleSidebar()
            }

            Row {
                spacing: Theme.spacingXS
                Text {
                    textFormat: Text.PlainText
                    text: "oma"
                    color: Theme.mutedText
                    font.pixelSize: Theme.fontSize + 4
                    font.weight: Font.Light
                }
                Text {
                    textFormat: Text.PlainText
                    text: "calendar"
                    color: Theme.text
                    font.pixelSize: Theme.fontSize + 4
                    font.weight: Font.Bold
                }
            }

            Rectangle {
                Layout.leftMargin: Theme.spacingXS
                Layout.preferredWidth: 1
                Layout.preferredHeight: 28
                color: Theme.divider
            }

            AppButton {
                iconText: Theme.glyphPrevious
                quiet: true
                compact: true
                toolTipText: qsTr("Previous period  [")
                onClicked: window.navigatePeriod(-1)
            }
            AppButton {
                text: qsTr("Today")
                quiet: true
                compact: true
                onClicked: window.goToday()
            }
            AppButton {
                iconText: Theme.glyphNext
                quiet: true
                compact: true
                toolTipText: qsTr("Next period  ]")
                onClicked: window.navigatePeriod(1)
            }

            ColumnLayout {
                Layout.preferredWidth: 240
                spacing: 0
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: window.periodTitle()
                    color: Theme.text
                    font.pixelSize: Theme.fontSize + 2
                    font.weight: Font.Bold
                    elide: Text.ElideRight
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: window.periodSubtitle()
                    color: Theme.mutedText
                    font.pixelSize: Theme.microFontSize
                    elide: Text.ElideRight
                }
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                Layout.preferredWidth: viewSwitch.implicitWidth + Theme.spacingSM
                Layout.preferredHeight: 40
                radius: Theme.radiusMD
                color: Theme.background
                border.color: Theme.border
                RowLayout {
                    id: viewSwitch
                    anchors.centerIn: parent
                    spacing: 2
                    Repeater {
                        model: [
                            {"id": "agenda", "label": qsTr("Agenda"), "key": "1"},
                            {"id": "day", "label": qsTr("Day"), "key": "2"},
                            {"id": "week", "label": qsTr("Week"), "key": "3"},
                            {"id": "month", "label": qsTr("Month"), "key": "4"},
                            {"id": "year", "label": qsTr("Year"), "key": "5"}
                        ]
                        delegate: AppButton {
                            required property var modelData
                            text: modelData.label
                            compact: true
                            quiet: window.currentView !== modelData.id
                            primary: window.currentView === modelData.id
                            toolTipText: modelData.label + qsTr(" view  Alt+") + modelData.key
                            onClicked: window.setView(modelData.id)
                        }
                    }
                }
            }

            AppButton {
                iconText: Theme.glyphSearch
                quiet: true
                compact: true
                toolTipText: qsTr("Search  Ctrl+F")
                onClicked: window.openActivity("search")
            }

            Rectangle {
                id: cacheChip
                objectName: "cacheFreshnessChip"
                // The offline banner already explains a disconnected daemon, so
                // the freshness chip only speaks for a live but stale cache.
                visible: App.connected && window.width >= 1080
                Layout.preferredWidth: visible ? cacheChipText.implicitWidth
                                                 + Theme.spacingMD * 2 : 0
                Layout.preferredHeight: 26
                radius: Theme.radiusSM
                color: Theme.alpha(Theme.text, 0.05)
                border.color: Theme.divider
                Accessible.role: Accessible.StaticText
                Accessible.name: cacheChipText.text
                Text {
                    id: cacheChipText
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    text: window.cacheFreshnessText()
                    color: Theme.mutedText
                    font.pixelSize: Theme.microFontSize
                }
                ToolTip.visible: chipHover.hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Events are served from the local cache. Provider sync runs in the background.")
                HoverHandler { id: chipHover }
            }

            StatusBadge {
                text: App.connected ? (App.busy ? qsTr("Syncing") : qsTr("Connected")) : qsTr("Offline")
                tone: App.connected ? (App.busy ? "info" : "success") : "danger"
            }

            AppButton {
                text: qsTr("Quick add")
                quiet: true
                toolTipText: qsTr("Describe an event in one line  Q")
                onClicked: quickAdd.openEmpty()
            }

            AppButton {
                text: qsTr("New event")
                iconText: "+"
                primary: true
                onClicked: editor.openNew(App.selectedDate, 540)
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        CalendarSidebar {
            id: calendarSidebar
            // Driven by the animated width so the collapse reads as motion
            // instead of a jump; the item leaves the layout once it is closed.
            visible: Layout.preferredWidth > 1
            clip: true
            Layout.preferredWidth: window.sidebarVisible ? Theme.sidebarWidth : 0
            Layout.fillHeight: true
            Behavior on Layout.preferredWidth {
                NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
            }
            currentDate: App.selectedDate
            monthDate: window.visibleMonth
            calendars: window.sidebarCalendars
            calendarSets: window.calendarSets
            showWeekNumbers: window.showWeekNumbers
            calendarsModel: null
            calendarSetsModel: window.appValue("calendarSetsModel", null)
            activeSetId: window.activeCalendarSetId
            connected: App.connected
            invitationCount: window.invitations.length
            conflictCount: window.conflicts.length
            failedOperationCount: window.failedOperationCount()
            eventCountForDate: function(dateValue) {
                return window.eventsForDate(dateValue, window.visibleEvents).length
            }
            calendarIsVisible: function(calendarId) {
                return window.calendarIsVisible(calendarId)
            }
            onDateSelected: dateValue => {
                window.selectDate(dateValue)
                if (window.currentView === "year")
                    window.setView("day")
            }
            onMonthChanged: dateValue => {
                window.visibleMonth = dateValue
                window.loadRangeFor(App.selectedDate, window.currentView)
            }
            onSetActivated: setId => window.activateCalendarSet(setId)
            onCalendarVisibilityRequested: (calendarId, visible) =>
                                               window.setCalendarVisible(calendarId, visible)
            onPanelRequested: panelName => window.openActivity(panelName)
            onSettingsRequested: settingsDrawer.open()
            accounts: window.appList("accounts")
            accountSyncStates: window.appValue("accountSyncStates", ({}))
            onAccountReauthorizeRequested: accountId => window.callApp("reauthorizeAccount",
                                                                       [accountId])
            onAccountSyncRequested: accountId => window.callApp("syncAccount", [accountId])
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.background

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    visible: App.lastError.length > 0
                    Layout.fillWidth: true
                    implicitHeight: errorRow.implicitHeight + 18
                    color: Theme.alpha(Theme.danger, 0.1)
                    border.color: Theme.alpha(Theme.danger, 0.3)
                    RowLayout {
                        id: errorRow
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMD
                        anchors.rightMargin: Theme.spacingSM
                        anchors.topMargin: Theme.spacingSM
                        anchors.bottomMargin: Theme.spacingSM
                        spacing: Theme.spacingSM
                        StatusBadge { dotOnly: true; text: qsTr("Error"); tone: "danger" }
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: App.lastError
                            color: Theme.text
                            font.pixelSize: Theme.smallFontSize
                            wrapMode: Text.Wrap
                        }
                        AppButton {
                            visible: !App.connected
                            text: qsTr("Reconnect")
                            compact: true
                            onClicked: App.reconnect()
                        }
                        AppButton {
                            text: qsTr("Details")
                            compact: true
                            quiet: true
                            onClicked: window.openActivity("sync")
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    // Settle fade on view change. A true crossfade would need
                    // two views alive at once, which the StackLayout does not
                    // support without risking model lifetime.
                    NumberAnimation {
                        id: viewSwitchFade
                        target: viewStack
                        property: "opacity"
                        from: 0.4
                        to: 1.0
                        duration: 130
                        easing.type: Easing.OutCubic
                    }

                    StackLayout {
                        id: viewStack
                        anchors.fill: parent
                        anchors.leftMargin: window.currentView === "month"
                                            || window.currentView === "week"
                                            ? 0 : Theme.spacingXL
                        anchors.rightMargin: anchors.leftMargin
                        anchors.topMargin: window.currentView === "month"
                                           || window.currentView === "week"
                                           ? 0 : Theme.spacingLG
                        anchors.bottomMargin: anchors.topMargin
                        currentIndex: Math.max(0, window.currentViewIndex)
                        onCurrentIndexChanged: viewSwitchFade.restart()

                        Loader {
                            // Only the visible view exists, so an events change reaches one
                            // view instead of all five.
                            active: viewStack.currentIndex === 0
                            sourceComponent: Component {
                                AgendaView {
                                    currentDate: App.selectedDate
                                    dayCount: window.agendaDayCount
                                    onMoreDaysRequested: window.extendAgenda()
                                    events: window.visibleEvents
                                    selectedEventReference: window.selectedEventReference
                                    timeFormat: String(window.preferences.timeFormat || "system")
                                    onEventActivated: value => window.openEvent(value)
                                    onCreateRequested: dateValue => editor.openNew(dateValue, 540)
                                    onDateSelected: dateValue => window.selectDate(dateValue)
                                }
                            }
                        }

                        Loader {
                            active: viewStack.currentIndex === 1
                            sourceComponent: Component {
                                DayView {
                                    currentDate: App.selectedDate
                                    savedScrollY: window.dayScrollY
                                    onScrollPositionChanged: contentY => window.dayScrollY = contentY
                                    events: window.visibleEvents
                                    selectedEventReference: window.selectedEventReference
                                    workDayStart: Number(window.preferences.workDayStart || 8)
                                    secondaryTime: window.secondaryTime
                                    workDayEnd: Number(window.preferences.workDayEnd || 18)
                                    defaultDurationMinutes:
                                        Number(window.preferences.defaultDuration || 60)
                                    timeFormat: String(window.preferences.timeFormat || "system")
                                    onEventActivated: value => window.openEvent(value)
                                    onCreateRequested: (dateValue, startMinute,
                                                        durationMinutes) =>
                                                           editor.openNew(dateValue, startMinute,
                                                                          durationMinutes)
                                    onEventTimeChanged: (value, dateValue, startMinute, durationMinutes) =>
                                                            window.rescheduleEvent(value, dateValue,
                                                                                   startMinute,
                                                                                   durationMinutes)
                                    onEventDateChanged: (value, dateValue) =>
                                                            window.moveEventToDate(value,
                                                                                   dateValue)
                                }
                            }
                        }

                        Loader {
                            active: viewStack.currentIndex === 2
                            sourceComponent: Component {
                                WeekView {
                                    currentDate: App.selectedDate
                                    savedScrollY: window.weekScrollY
                                    onScrollPositionChanged: contentY => window.weekScrollY = contentY
                                    events: window.visibleEvents
                                    selectedEventReference: window.selectedEventReference
                                    firstDayOfWeek: window.firstDayOfWeek
                                    showWeekNumbers: window.showWeekNumbers
                                    workDayStart: Number(window.preferences.workDayStart || 8)
                                    secondaryTime: window.secondaryTime
                                    workDayEnd: Number(window.preferences.workDayEnd || 18)
                                    defaultDurationMinutes:
                                        Number(window.preferences.defaultDuration || 60)
                                    timeFormat: String(window.preferences.timeFormat || "system")
                                    onEventActivated: value => window.openEvent(value)
                                    onDateSelected: dateValue => {
                                        window.selectDate(dateValue)
                                        window.setView("day")
                                    }
                                    onCreateRequested: (dateValue, startMinute,
                                                        durationMinutes) =>
                                                           editor.openNew(dateValue, startMinute,
                                                                          durationMinutes)
                                    onEventTimeChanged: (value, dateValue, startMinute, durationMinutes) =>
                                                            window.rescheduleEvent(value, dateValue,
                                                                                   startMinute,
                                                                                   durationMinutes)
                                    onEventDateChanged: (value, dateValue) =>
                                                            window.moveEventToDate(value,
                                                                                   dateValue)
                                    onEventAllDayRequested: (value, dateValue) =>
                                                                window.moveEventToAllDay(value,
                                                                                         dateValue)
                                }
                            }
                        }

                        Loader {
                            active: viewStack.currentIndex === 3
                            sourceComponent: Component {
                                MonthView {
                                    currentDate: App.selectedDate
                                    events: window.visibleEvents
                                    selectedEventReference: window.selectedEventReference
                                    firstDayOfWeek: window.firstDayOfWeek
                                    showWeekNumbers: window.showWeekNumbers
                                    timeFormat: String(window.preferences.timeFormat || "system")
                                    onEventActivated: value => window.openEvent(value)
                                    onDateSelected: dateValue => window.selectDate(dateValue)
                                    onCreateRequested: dateValue => editor.openNew(dateValue, 540)
                                    onEventDateChanged: (value, dateValue) =>
                                                            window.moveEventToDate(value, dateValue)
                                }
                            }
                        }

                        Loader {
                            active: viewStack.currentIndex === 4
                            sourceComponent: Component {
                                YearView {
                                    currentDate: App.selectedDate
                                    events: window.visibleEvents
                                    onDateSelected: dateValue => {
                                        window.selectDate(dateValue)
                                        window.setView("day")
                                    }
                                    onMonthSelected: dateValue => {
                                        window.selectDate(dateValue)
                                        window.setView("month")
                                    }
                                }
                            }
                        }
                    }

                    EmptyState {
                        visible: App.calendars.length === 0 && !App.busy
                        anchors.centerIn: parent
                        width: Math.min(420, parent.width - 60)
                        iconText: Theme.glyphCalendar
                        title: qsTr("Your calendar, locally first")
                        description: qsTr("Create a device-only calendar or connect Google, CalDAV, or an ICS subscription.")
                        actionText: qsTr("Add a calendar")
                        onActionRequested: settingsDrawer.openAccounts()
                    }

                    Rectangle {
                        visible: App.busy && !window.firstLoadComplete
                        anchors.fill: parent
                        color: Theme.alpha(Theme.background, 0.78)
                        BusyIndicator {
                            anchors.centerIn: parent
                            running: true
                        }
                    }

                    UndoToast {
                        id: undoToast
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: Theme.spacingLG
                        z: 50
                        onUndoRequested: window.callApp("undo", [])
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.darkBackground
                    border.color: Theme.divider
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMD
                        anchors.rightMargin: Theme.spacingMD
                        spacing: Theme.spacingSM
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: App.busy ? qsTr("Working…") : App.statusText
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                            elide: Text.ElideRight
                        }
                        Text {
                            textFormat: Text.PlainText
                            text: window.visibleEvents.length + qsTr(" events loaded")
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                        }
                        Rectangle {
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: 12
                            color: Theme.divider
                        }
                        Text {
                            textFormat: Text.PlainText
                            text: OmarchyTheme.sourceName
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                        }
                    }
                }
            }
        }
    }

    EventEditor {
        id: editor
        defaultDurationMinutes: Number(window.preferences.defaultDuration || 60)
        defaultCalendarId: String(window.preferences.defaultCalendarId || "")
        onSaveRequested: (value, options) => window.saveEvent(value, options)
        onRemoveRequested: (eventId, options) => window.removeEvent(eventId, options)
        onDuplicateRequested: value => window.duplicateEvent(value)
        onExportRequested: eventId => window.beginEventExport(eventId)
        onJoinRequested: url => App.openExternalEventUrl(url)
    }

    QuickAddDialog {
        id: quickAdd
        onDraftAccepted: draft => editor.openDraft(draft, App.selectedDate)
    }

    MutationConfirmationDialog {
        id: mutationConfirmation
        onConfirmed: (context, options) => {
            if (context.kind === "delete") {
                window.removeEvent(String(context.eventId || ""), options)
                return
            }
            if (context.kind === "save")
                window.performInteractionMutation(context.draft || ({}), options)
        }
    }

    Dialog {
        id: crossAccountMoveDialog
        anchors.centerIn: Overlay.overlay
        width: Math.min(470, Overlay.overlay ? Overlay.overlay.width - 48 : 470)
        height: 190
        modal: true
        title: qsTr("Move event to another account?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        closePolicy: Popup.CloseOnEscape

        contentItem: Text {
            textFormat: Text.PlainText
            width: crossAccountMoveDialog.availableWidth
            text: qsTr("OmaCalendar will create the destination event first. It will delete the original only after the destination provider acknowledges it.")
            color: Theme.text
            wrapMode: Text.Wrap
            font.pixelSize: Theme.smallFontSize
        }

        onAccepted: {
            const confirmedOptions = Object.assign({}, window.pendingMoveOptions,
                                                   {"confirmedCrossProvider": true})
            const value = window.pendingMoveEvent
            window.pendingMoveEvent = ({})
            window.pendingMoveOptions = ({})
            window.saveEvent(value, confirmedOptions)
        }
        onRejected: {
            window.pendingMoveEvent = ({})
            window.pendingMoveOptions = ({})
        }
    }

    ConflictMergeDialog {
        id: conflictMergeDialog
    }

    ActivityPanel {
        id: activityPanel
        mode: "search"
        searchResults: window.searchResults
        invitations: window.invitations
        conflicts: window.conflicts
        operations: window.operations
        searchResultsModel: App.connected
                            ? window.appValue("searchResultsModel", null) : null
        invitationsModel: window.appValue("invitationsModel", null)
        conflictsModel: window.appValue("conflictsModel", null)
        operationsModel: window.appValue("operationsModel", null)
        calendars: App.calendars
        accounts: App.accounts
        connected: App.connected
        syncing: App.busy
        statusText: App.statusText
        timeFormat: String(window.preferences.timeFormat || "system")
        onSearchRequested: (query, filters) => window.search(query, filters)
        onEventActivated: value => window.openEvent(value)
        onInvitationResponseRequested: (invitationId, recurrenceId,
                                        expectedLocalRevision, response,
                                        recurrenceScope) =>
                                                   window.callApp("respondToInvitation",
                                                                  [invitationId,
                                                                   response,
                                                                   recurrenceScope,
                                                                   recurrenceId,
                                                                   expectedLocalRevision])
        onInvitationSeenRequested: invitationId =>
                                            window.callApp("markInvitationSeen",
                                                           [invitationId])
        onConflictResolutionRequested: (conflictId, strategy, mergedDraft) =>
                                                   window.callApp("resolveConflict",
                                                                  [conflictId,
                                                                   strategy,
                                                                   mergedDraft])
        onConflictMergeRequested: conflictData =>
                                              conflictMergeDialog.openFor(
                                                  conflictData)
        onOperationRetryRequested: operationId =>
                                               window.callApp("retryOperation",
                                                              [operationId])
        onOperationDiscardRequested: operationId =>
                                                 window.callApp("discardOperation",
                                                                [operationId])
        onSyncRequested: App.syncAll()
    }

    AccountSettingsDrawer {
        id: settingsDrawer
        accounts: App.accounts
        calendars: App.calendars
        calendarSets: window.calendarSets
        accountsModel: window.appValue("accountsModel", null)
        calendarsModel: window.appValue("calendarsModel", null)
        calendarSetsModel: window.appValue("calendarSetsModel", null)
        connected: App.connected
        busy: App.busy
        statusText: App.statusText
        lastError: App.lastError
        preferences: window.preferences
        systemTimeZoneId: String(window.appValue("systemTimeZoneId", "UTC"))
        availableTimeZoneIds: window.appValue("availableTimeZoneIds", ["UTC"])
        bundledGoogleOAuthAvailable: Boolean(
                                           window.appValue(
                                               "bundledGoogleOAuthAvailable",
                                               false))
        googleOAuthConfigured: Boolean(
                                   window.appValue("googleOAuthConfigured", false))
        onConnectGoogleRequested: displayName => {
            if (settingsDrawer.bundledGoogleOAuthAvailable)
                App.connectGoogle(displayName)
            else
                App.connectGoogleConfigured(displayName)
        }
        onConnectGoogleClientRequested: (clientId, displayName) =>
                                            App.connectGoogleWithClientId(
                                                clientId, displayName)
        onConnectGoogleCredentialsRequested: displayName => {
            window.pendingGoogleDisplayName = displayName
            googleCredentialsFileDialog.open()
        }
        onAddCalDavRequested: (endpoint, username, password, displayName) =>
                                      App.addCalDavAccount(endpoint, username,
                                                          password, displayName)
        onAddLocalCalendarRequested: (name, color, muteAlerts) =>
                                             window.callApp("addLocalCalendar",
                                                            [name, color,
                                                             muteAlerts])
        onRemoveCalendarRequested: calendarId => App.removeCalendar(calendarId)
        onAddIcsSubscriptionRequested: (url, username, password, displayName) =>
                                               window.callApp("addIcsSubscription",
                                                              [{"url": url,
                                                                "username": username,
                                                                "password": password,
                                                                "displayName": displayName}])
        onRemoveAccountRequested: (accountId, removeCachedData) => {
            if (!window.callApp("removeAccountWithOptions",
                                [accountId, removeCachedData]))
                App.removeAccount(accountId)
        }
        onReauthorizeAccountRequested: accountId => App.reauthorizeAccount(accountId)
        onUpdateAccountCredentialsRequested: (accountId, username, password) =>
                                                 App.updateAccountCredentials(
                                                     accountId, username, password)
        onSyncAccountRequested: accountId => App.syncAccount(accountId)
        onCalendarPreferenceChanged: (calendarId, key, value) =>
                                                 window.callApp("setCalendarPreference",
                                                                [calendarId, key, value])
        onPreferenceChanged: (key, value) =>
                                     window.callApp("setPreference", [key, value])
        onDiagnosticsRequested: window.callApp("previewDiagnostics", [])
        onImportIcsRequested: importIcsFileDialog.open()
        onExportIcsRequested: icsExportDialog.openForSelection()
        onUpsertCalendarSetRequested: value => App.upsertCalendarSet(value)
        onRemoveCalendarSetRequested: setId => App.removeCalendarSet(setId)
    }

    FileDialog {
        id: googleCredentialsFileDialog
        title: qsTr("Choose Google desktop OAuth credentials")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("JSON files (*.json)"), qsTr("All files (*)")]
        onAccepted: App.connectGoogleWithCredentials(
                        selectedFile, window.pendingGoogleDisplayName)
    }

    FileDialog {
        id: importIcsFileDialog
        title: qsTr("Choose an iCalendar file")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("iCalendar files (*.ics)"), qsTr("All files (*)")]
        onAccepted: window.openIcsImport(selectedFile)
    }

    IcsImportDialog {
        id: icsImportDialog
        writableCalendars: window.writableCalendars
    }

    IcsExportDialog {
        id: icsExportDialog
        calendarSets: window.calendarSets
        localWritableCalendars: window.localWritableCalendars
        activeCalendarSetIndex: window.calendarSetIndex(window.activeCalendarSetId)
        onScopeChosen: (scopeIndex, calendarSetId, calendarId, rangeStart, rangeEnd) => {
            const scope = window.exportScope(scopeIndex, calendarSetId, calendarId,
                                             rangeStart, rangeEnd)
            if (Object.keys(scope).length > 0) {
                window.pendingExportScope = scope
                icsExportDialog.close()
                exportIcsFileDialog.open()
            }
        }
    }

    FileDialog {
        id: exportIcsFileDialog
        title: qsTr("Save iCalendar export")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "ics"
        nameFilters: [qsTr("iCalendar files (*.ics)")]
        onAccepted: App.exportIcs(window.pendingExportScope, selectedFile)
    }

    Connections {
        target: App
        function onIcsImportPreviewReady(preview) {
            icsImportDialog.preview = preview
        }
        function onIcsImportCompleted(result) {
            icsImportDialog.close()
        }
        function onOpenIcsImportRequested(file) {
            window.openIcsImport(file)
        }
    }

    Shortcut {
        sequence: "Ctrl+N"
        context: Qt.ApplicationShortcut
        onActivated: editor.openNew(App.selectedDate, 540)
    }
    Shortcut {
        sequences: ["Ctrl+Shift+N", "Q"]
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: quickAdd.openEmpty()
    }
    Shortcut {
        sequence: "Ctrl+F"
        context: Qt.ApplicationShortcut
        onActivated: window.openActivity("search")
    }
    Shortcut {
        sequence: "/"
        enabled: !editor.opened && !settingsDrawer.opened
        context: Qt.ApplicationShortcut
        onActivated: window.openActivity("search")
    }
    Shortcut { sequence: "Alt+1"; context: Qt.ApplicationShortcut; onActivated: window.setView("agenda") }
    Shortcut { sequence: "Alt+2"; context: Qt.ApplicationShortcut; onActivated: window.setView("day") }
    Shortcut { sequence: "Alt+3"; context: Qt.ApplicationShortcut; onActivated: window.setView("week") }
    Shortcut { sequence: "Alt+4"; context: Qt.ApplicationShortcut; onActivated: window.setView("month") }
    Shortcut { sequence: "Alt+5"; context: Qt.ApplicationShortcut; onActivated: window.setView("year") }
    Shortcut {
        sequence: "T"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.goToday()
    }
    Shortcut {
        sequence: "["
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.navigatePeriod(-1)
    }
    Shortcut {
        sequence: "]"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.navigatePeriod(1)
    }
    Shortcut {
        sequence: "Left"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.moveSelectionDate(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.moveSelectionDate(1)
    }
    Shortcut {
        sequence: "Up"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.selectAdjacentEvent(-1)
    }
    Shortcut {
        sequence: "Down"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.selectAdjacentEvent(1)
    }
    Shortcut {
        sequence: "Return"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.openKeyboardSelection()
    }
    Shortcut {
        sequence: "Enter"
        enabled: window.navigationShortcutsEnabled()
        context: Qt.ApplicationShortcut
        onActivated: window.openKeyboardSelection()
    }
    // Keyboard move and resize for the selected timed event, matching drag:
    // 15-minute steps, a day at a time sideways, and the same recurrence
    // scope prompt.
    Shortcut {
        sequence: "Alt+Up"
        enabled: window.navigationShortcutsEnabled() && window.canNudgeSelection()
        context: Qt.ApplicationShortcut
        onActivated: window.nudgeSelectedEvent(-15, 0, 0)
    }
    Shortcut {
        sequence: "Alt+Down"
        enabled: window.navigationShortcutsEnabled() && window.canNudgeSelection()
        context: Qt.ApplicationShortcut
        onActivated: window.nudgeSelectedEvent(15, 0, 0)
    }
    Shortcut {
        sequence: "Alt+Shift+Up"
        enabled: window.navigationShortcutsEnabled() && window.canNudgeSelection()
        context: Qt.ApplicationShortcut
        onActivated: window.nudgeSelectedEvent(0, -15, 0)
    }
    Shortcut {
        sequence: "Alt+Shift+Down"
        enabled: window.navigationShortcutsEnabled() && window.canNudgeSelection()
        context: Qt.ApplicationShortcut
        onActivated: window.nudgeSelectedEvent(0, 15, 0)
    }
    Shortcut {
        sequence: "Alt+Left"
        enabled: window.navigationShortcutsEnabled() && window.canNudgeSelection()
        context: Qt.ApplicationShortcut
        onActivated: window.nudgeSelectedEvent(0, 0, -1)
    }
    Shortcut {
        sequence: "Alt+Right"
        enabled: window.navigationShortcutsEnabled() && window.canNudgeSelection()
        context: Qt.ApplicationShortcut
        onActivated: window.nudgeSelectedEvent(0, 0, 1)
    }

    // Screen readers hear the view and period after navigation settles, once
    // the period's events have loaded.
    Item {
        id: announcer
        objectName: "announcer"
        property string lastAnnouncement: ""
        property bool pending: false
        Accessible.role: Accessible.StaticText
        Accessible.name: lastAnnouncement
    }
    Timer {
        id: announceTimer
        interval: 350
        onTriggered: {
            announcer.pending = false
            window.announce(window.viewAnnouncement())
        }
    }
    onCurrentViewChanged: window.queueViewAnnouncement()
    onVisibleEventsChanged: {
        if (announcer.pending)
            announceTimer.restart()
    }
    Connections {
        target: App
        ignoreUnknownSignals: true
        function onSelectedDateChanged() { window.queueViewAnnouncement() }
    }

    Shortcut {
        sequence: "Delete"
        enabled: window.navigationShortcutsEnabled()
                 && Boolean(window.selectedEvent.id)
        context: Qt.ApplicationShortcut
        onActivated: window.requestKeyboardDelete()
    }
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: !editor.opened
        context: Qt.ApplicationShortcut
        onActivated: {
            undoToast.dismiss()
            window.callApp("undo", [])
        }
    }
    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        enabled: !editor.opened
        context: Qt.ApplicationShortcut
        onActivated: {
            undoToast.dismiss()
            window.callApp("redo", [])
        }
    }
    Connections {
        target: App
        ignoreUnknownSignals: true
        function onMutationCompleted(message, undoable) {
            undoToast.show(message, undoable)
            window.announce(undoable ? qsTr("%1. Press Control Z to undo.").arg(message)
                                     : message)
        }
    }

    Connections {
        target: App
        function onEventsChanged() {
            window.firstLoadComplete = true
            window.reconcileSelectedEvent()
            if (activityPanel.searchText.length > 0)
                window.search(activityPanel.searchText,
                              window.activeSearchFilters)
        }
        function onCalendarsChanged() {
            window.calendarVisibilityOverrides = ({})
        }
        function onSelectedDateChanged() {
            window.visibleMonth = new Date(App.selectedDate.getFullYear(),
                                           App.selectedDate.getMonth(), 1)
        }
        function onPreferencesChanged() {
            window.applyInitialPreferences()
        }
        function onOpenEventRequested(eventData) {
            window.openEvent(eventData)
        }
        function onCreateEventRequested(draft) {
            editor.openExisting(draft)
        }
        function onOpenSectionRequested(section) {
            if (section === "invitations")
                window.openActivity(section)
            else
                settingsDrawer.open()
        }
        function onWindowActivationRequested() {
            window.showNormal()
            window.raise()
            window.requestActivate()
        }
    }

    Component.onCompleted: {
        applyInitialPreferences()
        loadRangeFor(App.selectedDate, currentView)
    }

    function toggleSidebar() {
        sidebarVisible = !sidebarVisible
        // An explicit toggle wins over the width rule until the next crossing.
        sidebarAutoHidden = false
    }

    function updateSidebarForWidth() {
        if (width < Theme.sidebarCollapseWidth) {
            if (sidebarVisible) {
                sidebarVisible = false
                sidebarAutoHidden = true
            }
        } else if (sidebarAutoHidden) {
            sidebarVisible = true
            sidebarAutoHidden = false
        }
    }

    function latestCalendarSync() {
        const values = appList("calendars")
        let latest = null
        for (let index = 0; index < values.length; ++index) {
            const raw = String(values[index].lastSyncAt || "")
            if (raw.length === 0)
                continue
            const parsed = new Date(raw)
            if (isNaN(parsed.getTime()))
                continue
            if (latest === null || parsed.getTime() > latest.getTime())
                latest = parsed
        }
        return latest
    }

    function cacheFreshnessText() {
        // clockTick keeps the relative label refreshing once a minute.
        const tick = clockTick
        const latest = latestSyncDate
        if (latest === null)
            return qsTr("Cached · never synced")
        return qsTr("Cached · synced ") + Theme.relativeSince(latest, new Date())
    }

    function appValue(name, fallbackValue) {
        try {
            const value = App[name]
            return value === undefined || value === null ? fallbackValue : value
        } catch (error) {
            return fallbackValue
        }
    }

    function viewIndex(viewName) {
        if (viewName === "agenda")
            return 0
        if (viewName === "day")
            return 1
        if (viewName === "week")
            return 2
        if (viewName === "month")
            return 3
        if (viewName === "year")
            return 4
        return 3
    }

    function appList(name) {
        const value = appValue(name, [])
        return value && typeof value.length === "number" ? value : []
    }

    function appObject(name) {
        const value = appValue(name, {})
        return value && typeof value === "object" ? value : {}
    }

    function callApp(methodName, argumentsList) {
        try {
            const method = App[methodName]
            if (typeof method === "function") {
                method.apply(App, argumentsList || [])
                return true
            }
        } catch (error) {
            console.warn("App method unavailable:", methodName, error)
        }
        return false
    }

    function calendarFor(calendarId) {
        for (let index = 0; index < App.calendars.length; ++index) {
            if (App.calendars[index].id === calendarId)
                return App.calendars[index]
        }
        return {}
    }

    function decorateEvents(values) {
        const result = []
        for (let index = 0; index < values.length; ++index) {
            const value = Object.assign({}, values[index])
            const calendar = calendarFor(value.calendarId)
            value.calendarName = calendar.name || qsTr("Calendar")
            value.calendarColor = calendar.colorOverride || calendar.color || Theme.accent
            value.readOnly = value.readOnly === true || calendar.readOnly === true
            result.push(value)
        }
        return result
    }

    function filterVisibleEvents(values) {
        return CalendarVisibility.filterEvents(
                    values, appList("calendars"), calendarSets,
                    activeCalendarSetId, calendarVisibilityOverrides)
    }

    function activeSetCalendarIds() {
        return CalendarVisibility.activeSetCalendarIds(calendarSets,
                                                        activeCalendarSetId)
    }

    function calendarIsVisible(calendarId) {
        return CalendarVisibility.calendarIsVisible(
                    appList("calendars"), calendarVisibilityOverrides,
                    calendarId)
    }

    function setCalendarVisible(calendarId, visibleValue) {
        const next = Object.assign({}, calendarVisibilityOverrides)
        next[String(calendarId)] = visibleValue === true
        calendarVisibilityOverrides = next
        callApp("setCalendarVisibility", [calendarId, visibleValue])
    }

    function activateCalendarSet(setId) {
        callApp("activateCalendarSet", [String(setId || "")])
    }

    function applyInitialPreferences() {
        const savedView = String(preferences.currentView || "month")
        if (["agenda", "day", "week", "month", "year"].indexOf(savedView) >= 0
                && currentView !== savedView) {
            currentView = savedView
            loadRangeFor(App.selectedDate, savedView)
        }
    }

    function eventStart(value) {
        return EventIndex.eventStart(value)
    }

    function eventEnd(value) {
        return EventIndex.eventEnd(value)
    }

    function eventsForDate(dateValue, values) {
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                               dateValue.getDate())
        const end = new Date(start.getFullYear(), start.getMonth(), start.getDate() + 1)
        const result = []
        for (let index = 0; index < values.length; ++index) {
            if (eventStart(values[index]) < end && eventEnd(values[index]) > start)
                result.push(values[index])
        }
        result.sort(function(first, second) {
            if (first.allDay !== second.allDay)
                return first.allDay ? -1 : 1
            return eventStart(first) - eventStart(second)
        })
        return result
    }

    function eventReference(value) {
        if (!value || !value.id)
            return ""
        return String(value.id) + "\n" + String(value.recurrenceId || "")
    }

    function selectedDateEvents() {
        return eventsForDate(App.selectedDate, visibleEvents)
    }

    function reconcileSelectedEvent() {
        const reference = selectedEventReference
        if (!reference)
            return
        for (let index = 0; index < visibleEvents.length; ++index) {
            if (eventReference(visibleEvents[index]) === reference) {
                selectedEvent = visibleEvents[index]
                return
            }
        }
        selectedEvent = ({})
    }

    function selectAdjacentEvent(direction) {
        if (currentView === "year") {
            moveSelectionDate(direction * 7)
            return
        }
        const candidates = selectedDateEvents()
        if (candidates.length === 0) {
            selectedEvent = ({})
            return
        }
        const reference = selectedEventReference
        let selectedIndex = -1
        for (let index = 0; index < candidates.length; ++index) {
            if (eventReference(candidates[index]) === reference) {
                selectedIndex = index
                break
            }
        }
        if (selectedIndex < 0)
            selectedIndex = direction > 0 ? 0 : candidates.length - 1
        else
            selectedIndex = Math.max(0, Math.min(candidates.length - 1,
                                                 selectedIndex + direction))
        selectedEvent = candidates[selectedIndex]
    }

    function moveSelectionDate(days) {
        const next = new Date(App.selectedDate.getFullYear(),
                              App.selectedDate.getMonth(),
                              App.selectedDate.getDate() + days)
        selectedEvent = ({})
        selectDate(next)
    }

    function openKeyboardSelection() {
        if (selectedEventReference) {
            openEvent(selectedEvent)
            return
        }
        const candidates = selectedDateEvents()
        if (candidates.length > 0) {
            selectedEvent = candidates[0]
            openEvent(selectedEvent)
        }
    }

    function selectDate(dateValue) {
        App.setSelectedDate(dateValue)
        visibleMonth = new Date(dateValue.getFullYear(), dateValue.getMonth(), 1)
        loadRangeFor(dateValue, currentView)
    }

    function extendAgenda() {
        // Bounded so a runaway scroll cannot request an unbounded range.
        if (agendaDayCount >= 366)
            return
        agendaDayCount += 31
        loadRangeFor(App.selectedDate, "agenda")
    }

    function setView(viewName) {
        currentView = viewName
        let anchorDate = App.selectedDate
        if (viewName === "agenda") {
            anchorDate = new Date()
            agendaDayCount = 31
            App.setSelectedDate(anchorDate)
            visibleMonth = new Date(anchorDate.getFullYear(),
                                    anchorDate.getMonth(), 1)
        }
        callApp("setCurrentView", [viewName])
        loadRangeFor(anchorDate, viewName)
    }

    function loadRangeFor(anchorDate, viewName) {
        let start
        let end
        if (viewName === "day") {
            start = new Date(anchorDate.getFullYear(), anchorDate.getMonth(),
                             anchorDate.getDate() - 1)
            end = new Date(anchorDate.getFullYear(), anchorDate.getMonth(),
                           anchorDate.getDate() + 2)
        } else if (viewName === "week") {
            start = startOfWeek(anchorDate)
            start.setDate(start.getDate() - 1)
            end = new Date(start.getFullYear(), start.getMonth(), start.getDate() + 10)
        } else if (viewName === "agenda") {
            start = new Date(anchorDate.getFullYear(), anchorDate.getMonth(),
                             anchorDate.getDate() - 7)
            end = new Date(anchorDate.getFullYear(), anchorDate.getMonth(),
                           anchorDate.getDate() + agendaDayCount + 14)
        } else if (viewName === "year") {
            start = new Date(anchorDate.getFullYear(), 0, 1)
            end = new Date(anchorDate.getFullYear(), 11, 31)
        } else {
            start = new Date(anchorDate.getFullYear(), anchorDate.getMonth(), -7)
            end = new Date(anchorDate.getFullYear(), anchorDate.getMonth() + 1, 14)
        }
        const requestedRange = DateRange.includeMonthGrid(
                                 start, end, visibleMonth, firstDayOfWeek)
        App.loadRange(requestedRange.start, requestedRange.end)
    }

    function startOfWeek(dateValue) {
        return DateRange.startOfWeek(dateValue, firstDayOfWeek)
    }

    function navigatePeriod(direction) {
        const dateValue = new Date(App.selectedDate.getFullYear(),
                                   App.selectedDate.getMonth(),
                                   App.selectedDate.getDate())
        if (currentView === "day")
            dateValue.setDate(dateValue.getDate() + direction)
        else if (currentView === "week" || currentView === "agenda")
            dateValue.setDate(dateValue.getDate() + 7 * direction)
        else if (currentView === "year")
            dateValue.setFullYear(dateValue.getFullYear() + direction)
        else
            dateValue.setMonth(dateValue.getMonth() + direction)
        selectDate(dateValue)
    }

    function goToday() {
        selectDate(new Date())
    }

    function periodTitle() {
        if (currentView === "day")
            return Qt.formatDate(App.selectedDate, "dddd, MMMM d")
        if (currentView === "week") {
            const start = startOfWeek(App.selectedDate)
            const end = new Date(start.getFullYear(), start.getMonth(), start.getDate() + 6)
            return start.getMonth() === end.getMonth()
                    ? Qt.formatDate(start, "MMMM d") + "–" + end.getDate()
                    : Qt.formatDate(start, "MMM d") + " – " + Qt.formatDate(end, "MMM d")
        }
        if (currentView === "year")
            return String(App.selectedDate.getFullYear())
        return Qt.formatDate(App.selectedDate, "MMMM yyyy")
    }

    function periodSubtitle() {
        if (currentView === "agenda")
            return qsTr("Upcoming agenda")
        if (currentView === "month")
            return Qt.formatDate(App.selectedDate, "dddd, MMMM d")
        if (currentView === "year")
            return qsTr("Year overview")
        if (currentView === "week")
            return qsTr("Week view")
        return qsTr("Day view")
    }

    function openEvent(value) {
        selectedEvent = value
        editor.openExisting(value)
    }

    function saveEvent(value, options) {
        const mutationOptions = Object.assign({}, options || {})
        const sourceCalendarId = String(mutationOptions.sourceCalendarId || "")
        const targetCalendarId = String(value.calendarId || "")
        if (value.id && sourceCalendarId && targetCalendarId
                && sourceCalendarId !== targetCalendarId
                && calendarAccountId(sourceCalendarId)
                   !== calendarAccountId(targetCalendarId)
                && mutationOptions.confirmedCrossProvider !== true) {
            pendingMoveEvent = value
            pendingMoveOptions = mutationOptions
            crossAccountMoveDialog.open()
            return
        }
        if (callApp("saveEvent", [value, mutationOptions]))
            return
        if (value.id)
            App.updateEvent(value)
        else
            App.createEvent(value)
    }

    function calendarAccountId(calendarId) {
        const values = appList("calendars")
        for (let index = 0; index < values.length; ++index) {
            if (String(values[index].id) === String(calendarId))
                return String(values[index].accountId || "")
        }
        return ""
    }

    function accountProvider(accountId) {
        const values = appList("accounts")
        for (let index = 0; index < values.length; ++index) {
            if (String(values[index].id) === String(accountId))
                return String(values[index].provider || "")
        }
        return ""
    }

    function calendarSetIndex(setId) {
        for (let index = 0; index < calendarSets.length; ++index) {
            if (String(calendarSets[index].id) === String(setId))
                return index
        }
        return 0
    }

    function openIcsImport(fileUrl) {
        icsImportDialog.fileUrl = fileUrl
        icsImportDialog.preview = ({})
        icsImportDialog.open()
    }

    function exportScope(scopeIndex, calendarSetId, calendarId,
                         startText, endText) {
        if (scopeIndex === 1)
            return {"calendarSetId": String(calendarSetId
                                             || activeCalendarSetId)}
        if (scopeIndex === 2)
            return calendarId ? {"calendarId": String(calendarId)} : ({})
        const start = new Date(String(startText) + "T00:00:00")
        const inclusiveEnd = new Date(String(endText) + "T00:00:00")
        if (isNaN(start.getTime()) || isNaN(inclusiveEnd.getTime())
                || inclusiveEnd < start)
            return ({})
        inclusiveEnd.setDate(inclusiveEnd.getDate() + 1)
        return {"start": start.toISOString(), "end": inclusiveEnd.toISOString()}
    }

    function beginEventExport(eventId) {
        pendingExportScope = {"eventId": String(eventId)}
        editor.close()
        exportIcsFileDialog.open()
    }

    function removeEvent(eventId, options) {
        if (!callApp("requestDeleteEvent", [eventId, options]))
            App.removeEvent(eventId)
        selectedEvent = ({})
    }

    function duplicateEvent(value) {
        const copy = Object.assign({}, value)
        delete copy.id
        delete copy.remoteId
        delete copy.uid
        delete copy.etag
        delete copy.recurrenceId
        copy.summary = (copy.summary || qsTr("Untitled event")) + qsTr(" copy")
        copy.dirty = false
        copy.conflict = false
        editor.openExisting(copy)
    }

    function rescheduleEvent(value, dateValue, startMinute, durationMinutes) {
        if (!eventEditable(value))
            return
        const revision = Number(value.localRevision)
        if (!isFinite(revision) || revision < 0) {
            console.warn("Cannot reschedule event without a local revision")
            return
        }
        const updated = Object.assign({}, value)
        const start = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                               dateValue.getDate(), Math.floor(startMinute / 60),
                               startMinute % 60)
        updated.allDay = false
        if (value.allDay) {
            // Leaving the all-day lane: the event takes the display zone.
            updated.timeKind = "zoned"
            updated.startTimeZone = String(preferences.displayTimeZone
                                           || App.systemTimeZoneId || "UTC")
            updated.endTimeZone = updated.startTimeZone
        }
        updated.startUtc = utcForDisplayedWall(start, updated.timeKind)
        if (!updated.startUtc)
            return
        updated.endUtc = new Date(new Date(updated.startUtc).getTime()
                                  + durationMinutes * 60000).toISOString()
        updated.startDate = ""
        updated.endDate = ""
        updated.localRevision = revision
        submitInteractionMutation(updated, value)
    }

    // A timed event dropped on the all-day lane becomes a one-day all-day
    // event on that date.
    function moveEventToAllDay(value, dateValue) {
        if (!eventEditable(value) || value.allDay)
            return
        const revision = Number(value.localRevision)
        if (!isFinite(revision) || revision < 0) {
            console.warn("Cannot move event without a local revision")
            return
        }
        const updated = Object.assign({}, value)
        updated.allDay = true
        updated.timeKind = "all_day"
        updated.startDate = Qt.formatDate(dateValue, "yyyy-MM-dd")
        updated.endDate = Qt.formatDate(new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                                 dateValue.getDate() + 1), "yyyy-MM-dd")
        updated.startUtc = ""
        updated.endUtc = ""
        updated.localRevision = revision
        submitInteractionMutation(updated, value)
    }

    function moveEventToDate(value, dateValue) {
        if (!eventEditable(value))
            return
        const revision = Number(value.localRevision)
        if (!isFinite(revision) || revision < 0) {
            console.warn("Cannot move event without a local revision")
            return
        }
        const start = eventStart(value)
        const end = eventEnd(value)
        if (Theme.sameDate(start, dateValue))
            return
        const duration = end - start
        const updated = Object.assign({}, value)
        if (value.allDay) {
            updated.startDate = Qt.formatDate(dateValue, "yyyy-MM-dd")
            const days = Math.max(1, Math.round(duration / 86400000))
            const newEnd = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                    dateValue.getDate() + days)
            updated.endDate = Qt.formatDate(newEnd, "yyyy-MM-dd")
        } else {
            const newStart = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                      dateValue.getDate(), start.getHours(),
                                      start.getMinutes())
            updated.startUtc = utcForDisplayedWall(newStart, value.timeKind)
            if (!updated.startUtc)
                return
            updated.endUtc = new Date(new Date(updated.startUtc).getTime()
                                      + duration).toISOString()
        }
        updated.localRevision = revision
        submitInteractionMutation(updated, value)
    }

    function submitInteractionMutation(updated, source) {
        const options = {
            "expectedLocalRevision": Number(source.localRevision),
            "recurrenceScope": source.recurrenceId ? "occurrence" : "series",
            "guestNotificationPolicy": "none",
            "sourceCalendarId": String(source.calendarId || ""),
            "recurrenceId": String(source.recurrenceId || "")
        }
        if (mutationConfirmation.needsChoiceFor(source)) {
            mutationConfirmation.openFor(
                        source, qsTr("Apply change"),
                        {"kind": "save", "draft": updated}, options,
                        futureScopeSupportedForEvent(source))
            return
        }
        performInteractionMutation(updated, options)
    }

    function performInteractionMutation(updated, options) {
        if (!callApp("saveEvent", [updated, options]))
            console.warn("AppController.saveEvent is unavailable")
    }

    function requestKeyboardDelete() {
        const value = selectedEvent
        if (!value || !value.id || !eventEditable(value))
            return
        const options = {
            "expectedLocalRevision": Number(value.localRevision),
            "recurrenceScope": value.recurrenceId ? "occurrence" : "series",
            "guestNotificationPolicy": "none",
            "recurrenceId": String(value.recurrenceId || "")
        }
        if (mutationConfirmation.needsChoiceFor(value)) {
            mutationConfirmation.openFor(
                        value, qsTr("Delete event"),
                        {"kind": "delete", "eventId": String(value.id)}, options,
                        futureScopeSupportedForEvent(value))
            return
        }
        removeEvent(String(value.id), options)
    }

    function futureScopeSupportedForEvent(value) {
        const calendar = calendarFor(value.calendarId)
        const capabilities = calendar.capabilities || ({})
        return capabilities.thisAndFuture === true
    }

    function eventEditable(value) {
        if (!value || value.readOnly === true || value.conflict === true
                || value.dirty === true)
            return false
        const calendar = calendarFor(value.calendarId)
        if (calendar.readOnly === true)
            return false
        const operationState = String(value.operationState || value.syncState || "")
        return operationState !== "pending" && operationState !== "sending"
                && operationState !== "blocked" && operationState !== "retry_wait"
                && operationState !== "failed" && operationState !== "error"
    }

    function utcForDisplayedWall(dateValue, timeKind) {
        if (timeKind === "floating") {
            return new Date(Date.UTC(dateValue.getFullYear(), dateValue.getMonth(),
                                     dateValue.getDate(), dateValue.getHours(),
                                     dateValue.getMinutes(), 0)).toISOString()
        }
        return App.wallTimeToUtc(Qt.formatDate(dateValue, "yyyy-MM-dd"),
                                 Qt.formatTime(dateValue, "HH:mm"),
                                 String(preferences.displayTimeZone || ""))
    }

    function search(query, filters) {
        const normalized = String(query || "").trim().toLowerCase()
        if (normalized.length === 0) {
            localSearchResults = []
            activeSearchFilters = ({})
            return
        }
        const result = []
        const requestedFilters = filters || ({})
        activeSearchFilters = requestedFilters
        const calendarIds = requestedFilters.calendarIds || []
        const rangeStart = requestedFilters.start
                ? new Date(requestedFilters.start) : null
        const rangeEnd = requestedFilters.end
                ? new Date(requestedFilters.end) : null
        for (let index = 0; index < decoratedEvents.length; ++index) {
            const value = decoratedEvents[index]
            if (calendarIds.length > 0
                    && calendarIds.indexOf(String(value.calendarId)) < 0)
                continue
            if (requestedFilters.accountId
                    && calendarAccountId(value.calendarId)
                       !== String(requestedFilters.accountId))
                continue
            const start = eventStart(value)
            const end = eventEnd(value)
            if (rangeStart && end <= rangeStart)
                continue
            if (rangeEnd && start >= rangeEnd)
                continue
            const attendees = value.attendees || []
            if (requestedFilters.invitationState) {
                let matchingResponse = false
                for (let attendeeIndex = 0; attendeeIndex < attendees.length;
                     ++attendeeIndex) {
                    const response = String(attendees[attendeeIndex].responseStatus
                                            || attendees[attendeeIndex].partstat
                                            || "").toLowerCase()
                    if (response === String(requestedFilters.invitationState)
                                      .toLowerCase()) {
                        matchingResponse = true
                        break
                    }
                }
                if (!matchingResponse)
                    continue
            }
            let haystack = String(value.summary || "") + "\n"
                    + String(value.description || "") + "\n"
                    + String(value.location || "")
            for (let attendeeIndex = 0; attendeeIndex < attendees.length; ++attendeeIndex)
                haystack += "\n" + String(attendees[attendeeIndex].email || "")
            if (haystack.toLowerCase().indexOf(normalized) >= 0)
                result.push(value)
        }
        localSearchResults = result
        callApp("searchEvents", [query, filters || {}])
    }

    function openActivity(modeName) {
        activityPanel.mode = modeName
        activityPanel.open()
    }

    function failedOperationCount() {
        let count = 0
        for (let index = 0; index < operations.length; ++index) {
            if (operations[index].state === "blocked"
                    || operations[index].state === "retry_wait")
                ++count
        }
        return count
    }

    function queueViewAnnouncement() {
        announcer.pending = true
        announceTimer.restart()
    }

    function announce(message) {
        if (!message)
            return
        announcer.lastAnnouncement = message
        // Accessible.announce is available from Qt 6.8.
        if (typeof announcer.Accessible.announce === "function")
            announcer.Accessible.announce(message)
    }

    // [start, end) of the period the current view shows.
    function periodBounds() {
        const day = new Date(App.selectedDate.getFullYear(), App.selectedDate.getMonth(),
                             App.selectedDate.getDate())
        if (currentView === "day")
            return {"start": day, "end": new Date(day.getFullYear(), day.getMonth(),
                                                  day.getDate() + 1)}
        if (currentView === "week") {
            const start = startOfWeek(day)
            return {"start": start, "end": new Date(start.getFullYear(), start.getMonth(),
                                                    start.getDate() + 7)}
        }
        if (currentView === "agenda")
            return {"start": day, "end": new Date(day.getFullYear(), day.getMonth(),
                                                  day.getDate() + agendaDayCount)}
        if (currentView === "year")
            return {"start": new Date(day.getFullYear(), 0, 1),
                    "end": new Date(day.getFullYear() + 1, 0, 1)}
        return {"start": new Date(day.getFullYear(), day.getMonth(), 1),
                "end": new Date(day.getFullYear(), day.getMonth() + 1, 1)}
    }

    // For example "Week view, September 28–October 4, 12 events".
    function viewAnnouncement() {
        const bounds = periodBounds()
        let count = 0
        for (let index = 0; index < visibleEvents.length; ++index) {
            const value = visibleEvents[index]
            if (eventStart(value) < bounds.end && eventEnd(value) > bounds.start)
                ++count
        }
        const labels = {"agenda": qsTr("Agenda"), "day": qsTr("Day view"),
                        "week": qsTr("Week view"), "month": qsTr("Month view"),
                        "year": qsTr("Year view")}
        const period = currentView === "agenda"
                ? qsTr("from %1").arg(Qt.formatDate(bounds.start, "dddd, MMMM d"))
                : currentView === "day"
                  ? Qt.formatDate(bounds.start, "dddd, MMMM d") : periodTitle()
        return qsTr("%1, %2, %n event(s)", "", count).arg(labels[currentView] || "")
                .arg(period)
    }

    function canNudgeSelection() {
        return currentView !== "year" && Boolean(selectedEvent && selectedEvent.id)
                && eventEditable(selectedEvent)
    }

    // Moves the selected event by minutes and days, or changes its length by
    // resizeMinutes, through the same path as dragging it.
    function nudgeSelectedEvent(minutes, resizeMinutes, days) {
        const value = selectedEvent
        if (!canNudgeSelection())
            return
        const start = eventStart(value)
        const end = eventEnd(value)
        const title = value.summary || qsTr("Untitled event")
        const multiDay = value.allDay || !Theme.sameDate(start, new Date(end.getTime() - 1))
        if (multiDay) {
            if (days === 0)
                return
            const target = new Date(start.getFullYear(), start.getMonth(),
                                    start.getDate() + days)
            moveEventToDate(value, target)
            announce(qsTr("%1 moved to %2").arg(title)
                     .arg(Qt.formatDate(target, "dddd, MMMM d")))
            return
        }
        const duration = Math.round((end - start) / 60000)
        const nextDuration = Math.max(15, duration + resizeMinutes)
        if (resizeMinutes !== 0 && nextDuration === duration)
            return
        const moved = new Date(start.getFullYear(), start.getMonth(), start.getDate() + days,
                               start.getHours(), start.getMinutes() + minutes)
        rescheduleEvent(value, new Date(moved.getFullYear(), moved.getMonth(), moved.getDate()),
                        moved.getHours() * 60 + moved.getMinutes(), nextDuration)
        if (resizeMinutes !== 0) {
            const movedEnd = new Date(moved.getTime() + nextDuration * 60000)
            announce(qsTr("%1 now ends at %2").arg(title)
                     .arg(Theme.formatTime(movedEnd, String(preferences.timeFormat
                                                            || "system"))))
        } else {
            announce(qsTr("%1 moved to %2").arg(title)
                     .arg(Qt.formatDate(moved, "ddd MMM d") + " "
                          + Theme.formatTime(moved, String(preferences.timeFormat
                                                           || "system"))))
        }
    }

    function navigationShortcutsEnabled() {
        return !editor.opened && !settingsDrawer.opened && !activityPanel.opened
               && !quickAdd.opened
    }
}
