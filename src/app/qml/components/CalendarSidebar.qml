pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

Rectangle {
    id: root

    property date currentDate: new Date()
    property date monthDate: new Date(currentDate.getFullYear(), currentDate.getMonth(), 1)
    property var calendars: []
    property var calendarSets: []
    property var calendarsModel: null
    property var calendarSetsModel: null
    readonly property var effectiveCalendarsModel: calendarsModel || calendars
    readonly property var effectiveCalendarSetsModel: calendarSetsModel
                                                       || calendarSets
    property string activeSetId: ""
    property var eventCountForDate: function(dateValue) { return 0 }
    property var calendarIsVisible: function(calendarId) { return true }
    property int invitationCount: 0
    // Hidden when the calendar service has no tasks.
    property bool tasksAvailable: false
    // Open tasks due today or earlier.
    property int dueTaskCount: 0
    property int conflictCount: 0
    property int failedOperationCount: 0
    property var accounts: []
    property var accountSyncStates: ({})
    readonly property var syncedAccounts: (accounts || []).filter(function(account) {
        return account && account.provider !== "local"
    })
    property bool connected: false
    property bool showWeekNumbers: false

    signal dateSelected(date dateValue)
    signal monthChanged(date dateValue)
    signal setActivated(string setId)
    signal calendarVisibilityRequested(string calendarId, bool visible)
    signal panelRequested(string panelName)
    signal settingsRequested()
    signal accountReauthorizeRequested(string accountId)
    signal accountSyncRequested(string accountId)

    color: Theme.darkBackground
    border.color: Theme.divider

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacingLG
        anchors.rightMargin: Theme.spacingLG
        anchors.topMargin: Theme.spacingLG
        anchors.bottomMargin: Theme.spacingMD
        spacing: Theme.spacingMD

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingXS

            AppButton {
                iconText: "‹"
                quiet: true
                compact: true
                toolTipText: qsTr("Previous month")
                onClicked: {
                    root.monthDate = new Date(root.monthDate.getFullYear(),
                                              root.monthDate.getMonth() - 1, 1)
                    root.monthChanged(root.monthDate)
                }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: Qt.formatDate(root.monthDate, "MMMM yyyy")
                color: Theme.text
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Theme.fontSize
                font.weight: Font.DemiBold
            }
            AppButton {
                iconText: "›"
                quiet: true
                compact: true
                toolTipText: qsTr("Next month")
                onClicked: {
                    root.monthDate = new Date(root.monthDate.getFullYear(),
                                              root.monthDate.getMonth() + 1, 1)
                    root.monthChanged(root.monthDate)
                }
            }
        }

        DayOfWeekRow {
            Layout.fillWidth: true
            Layout.leftMargin: root.showWeekNumbers ? 22 : 0
            locale: Qt.locale()
            delegate: Text {
                textFormat: Text.PlainText
                required property string shortName
                text: shortName.slice(0, 1)
                color: Theme.mutedText
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Theme.microFontSize
                font.weight: Font.DemiBold
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 0

            WeekNumberColumn {
                objectName: "miniWeekNumbers"
                visible: root.showWeekNumbers
                Layout.preferredWidth: 22
                Layout.preferredHeight: 214
                month: miniMonth.month
                year: miniMonth.year
                locale: miniMonth.locale
                delegate: Text {
                    required property int weekNumber
                    textFormat: Text.PlainText
                    text: weekNumber
                    color: Theme.mutedText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: Theme.microFontSize
                }
            }

            MonthGrid {
                id: miniMonth
                Layout.fillWidth: true
                Layout.preferredHeight: 214
                month: root.monthDate.getMonth()
                year: root.monthDate.getFullYear()
                locale: Qt.locale()
                delegate: MonthCell {
                    required property var model
                    date: model.date
                    month: miniMonth.month
                    selected: root.sameDate(model.date, root.currentDate)
                    isToday: root.sameDate(model.date, new Date())
                    eventCount: root.eventCountForDate(model.date)
                    onClicked: root.dateSelected(model.date)
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.divider
        }

        RowLayout {
            Layout.fillWidth: true
            SectionLabel {
                Layout.fillWidth: true
                text: qsTr("CALENDAR SETS")
            }
            AppButton {
                visible: root.modelCount(root.effectiveCalendarSetsModel) > 0
                iconText: "+"
                quiet: true
                compact: true
                toolTipText: qsTr("Manage calendar sets")
                onClicked: root.settingsRequested()
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacingXS

            Repeater {
                model: root.modelCount(root.effectiveCalendarSetsModel) > 0
                       ? root.effectiveCalendarSetsModel
                       : [{"id": "", "name": qsTr("All calendars")}]
                delegate: AppButton {
                    required property var modelData
                    text: modelData.name || qsTr("Calendar set")
                    compact: true
                    quiet: String(modelData.id || "") !== root.activeSetId
                    primary: String(modelData.id || "") === root.activeSetId
                    onClicked: root.setActivated(String(modelData.id || ""))
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            SectionLabel {
                Layout.fillWidth: true
                text: qsTr("CALENDARS")
            }
            Text {
                textFormat: Text.PlainText
                text: root.modelCount(root.effectiveCalendarsModel)
                color: Theme.mutedText
                font.pixelSize: Theme.microFontSize
            }
        }

        ListView {
            id: calendarList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: root.effectiveCalendarsModel
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: ItemDelegate {
                id: calendarDelegate
                required property var modelData
                width: ListView.view.width
                height: 38
                padding: 0
                hoverEnabled: true
                Accessible.name: (root.calendarIsVisible(modelData.id)
                                  ? qsTr("Hide ") : qsTr("Show ")) + modelData.name
                onClicked: root.calendarVisibilityRequested(
                               modelData.id,
                               !root.calendarIsVisible(modelData.id))

                background: Rectangle {
                    radius: Theme.radiusMD
                    color: calendarDelegate.hovered || calendarDelegate.activeFocus
                           ? Theme.alpha(Theme.text, 0.065) : "transparent"
                    border.width: calendarDelegate.activeFocus ? 1 : 0
                    border.color: Theme.focus
                }
                contentItem: RowLayout {
                    spacing: Theme.spacingSM
                    Rectangle {
                        Layout.preferredWidth: 13
                        Layout.preferredHeight: 13
                        radius: Theme.radiusSM
                        color: root.calendarIsVisible(calendarDelegate.modelData.id)
                               ? (calendarDelegate.modelData.color || Theme.accent)
                               : "transparent"
                        border.width: 1
                        border.color: calendarDelegate.modelData.color || Theme.accent
                        Text {
                            textFormat: Text.PlainText
                            visible: root.calendarIsVisible(calendarDelegate.modelData.id)
                            anchors.centerIn: parent
                            text: "✓"
                            color: Theme.accentText
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: calendarDelegate.modelData.name || qsTr("Calendar")
                        color: root.calendarIsVisible(calendarDelegate.modelData.id)
                               ? Theme.text : Theme.mutedText
                        elide: Text.ElideRight
                        font.pixelSize: Theme.smallFontSize
                    }
                    Text {
                        textFormat: Text.PlainText
                        visible: calendarDelegate.modelData.readOnly === true
                        text: qsTr("Read only")
                        color: Theme.mutedText
                        font.pixelSize: Theme.microFontSize
                    }
                }
            }

            EmptyState {
                visible: calendarList.count === 0
                anchors.centerIn: parent
                width: Math.min(220, parent.width - 20)
                iconText: "◌"
                title: qsTr("No calendars")
                description: qsTr("Connect an account or create a local calendar.")
            }
        }

        SectionLabel {
            visible: root.syncedAccounts.length > 0
            Layout.fillWidth: true
            text: qsTr("ACCOUNTS")
        }

        Repeater {
            model: root.syncedAccounts
            delegate: RowLayout {
                id: accountRow
                required property var modelData
                required property int index
                readonly property var status: root.accountStatus(modelData)
                objectName: "accountSync-" + index
                Layout.fillWidth: true
                spacing: Theme.spacingSM
                Accessible.role: Accessible.StaticText
                Accessible.name: (modelData.displayName || modelData.principal || "")
                                 + ", " + status.text

                StatusBadge {
                    dotOnly: true
                    tone: accountRow.status.tone
                    text: accountRow.status.text
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: accountRow.modelData.displayName
                              || accountRow.modelData.principal || qsTr("Account")
                        color: Theme.text
                        elide: Text.ElideRight
                        font.pixelSize: Theme.smallFontSize
                    }
                    Text {
                        objectName: "accountSyncText-" + accountRow.index
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: accountRow.status.text
                        color: accountRow.status.tone === "danger" ? Theme.danger
                                                                   : Theme.mutedText
                        elide: Text.ElideRight
                        font.pixelSize: Theme.microFontSize
                        HoverHandler { id: statusHover }
                        ToolTip.visible: statusHover.hovered
                                         && accountRow.status.detail.length > 0
                        ToolTip.delay: 450
                        ToolTip.text: accountRow.status.detail
                    }
                }
                AppButton {
                    objectName: "accountSyncAction-" + accountRow.index
                    visible: accountRow.status.action.length > 0
                    compact: true
                    quiet: accountRow.status.action !== "reauthorize"
                    text: accountRow.status.action === "reauthorize" ? qsTr("Sign in")
                                                                    : qsTr("Retry")
                    toolTipText: accountRow.status.action === "reauthorize"
                                 ? qsTr("Authorize this account again")
                                 : qsTr("Sync this account now")
                    onClicked: {
                        if (accountRow.status.action === "reauthorize")
                            root.accountReauthorizeRequested(accountRow.modelData.id)
                        else
                            root.accountSyncRequested(accountRow.modelData.id)
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.divider
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            ItemDelegate {
                objectName: "sidebarActivity"
                visible: root.conflictCount + root.failedOperationCount > 0
                Layout.fillWidth: true
                implicitHeight: 38
                onClicked: root.panelRequested(root.conflictCount > 0 ? "conflicts" : "sync")
                Accessible.name: qsTr("Needs attention: %1 conflict(s), %2 stuck change(s)")
                                 .arg(root.conflictCount).arg(root.failedOperationCount)
                contentItem: RowLayout {
                    Text { textFormat: Text.PlainText; text: "!"; color: Theme.warning }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: qsTr("Needs attention")
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                    }
                    StatusBadge {
                        text: String(root.conflictCount + root.failedOperationCount)
                        tone: root.conflictCount > 0 ? "danger" : "warning"
                    }
                }
            }

            ItemDelegate {
                objectName: "sidebarTasks"
                Layout.fillWidth: true
                implicitHeight: 38
                visible: root.tasksAvailable
                text: qsTr("Tasks")
                onClicked: root.panelRequested("tasks")
                contentItem: RowLayout {
                    Text { textFormat: Text.PlainText; text: "☐"; color: Theme.mutedText }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: qsTr("Tasks")
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                    }
                    StatusBadge {
                        visible: root.dueTaskCount > 0
                        text: String(root.dueTaskCount)
                        tone: "warning"
                    }
                }
            }
            ItemDelegate {
                Layout.fillWidth: true
                implicitHeight: 38
                text: qsTr("Invitations")
                icon.name: "mail-unread-symbolic"
                onClicked: root.panelRequested("invitations")
                contentItem: RowLayout {
                    Text { textFormat: Text.PlainText; text: "◇"; color: Theme.mutedText }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: qsTr("Invitations")
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                    }
                    StatusBadge {
                        visible: root.invitationCount > 0
                        text: String(root.invitationCount)
                        tone: "info"
                    }
                }
            }
            ItemDelegate {
                Layout.fillWidth: true
                implicitHeight: 38
                onClicked: root.settingsRequested()
                contentItem: RowLayout {
                    Text { textFormat: Text.PlainText; text: "⚙"; color: Theme.mutedText }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: qsTr("Accounts & settings")
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                    }
                }
            }
        }
    }

    // {tone, text, detail, action} for an account row; action is
    // "reauthorize", "sync" or empty.
    function accountStatus(account) {
        const sync = (accountSyncStates || {})[account.id] || {}
        const state = String(sync.state || "")
        const detail = String(sync.message || "")
        if (state === "reauthorization_required"
                || account.authStatus === "reauthorization_required")
            return {"tone": "danger", "text": qsTr("Sign-in expired"),
                    "detail": detail, "action": "reauthorize"}
        if (state === "error")
            return {"tone": "danger", "text": qsTr("Sync failed"), "detail": detail,
                    "action": "sync"}
        if (state === "syncing" || state === "refreshing")
            return {"tone": "info", "text": qsTr("Syncing…"), "detail": "", "action": ""}
        if (state === "stale")
            return {"tone": "warning", "text": qsTr("Out of date"), "detail": detail,
                    "action": "sync"}
        if (account.enabled === false)
            return {"tone": "neutral", "text": qsTr("Paused"), "detail": "", "action": ""}
        const last = sync.lastSyncAt ? new Date(sync.lastSyncAt) : null
        if (!last || isNaN(last.getTime()))
            return {"tone": "neutral", "text": state.length > 0 ? qsTr("Not synced yet")
                                                                : qsTr("Checking…"),
                    "detail": "", "action": ""}
        const when = sameDate(last, new Date())
                ? Qt.formatTime(last, "HH:mm") : Qt.formatDate(last, "MMM d")
        return {"tone": "success", "text": qsTr("Synced %1").arg(when),
                "detail": Qt.formatDateTime(last, "yyyy-MM-dd HH:mm"), "action": ""}
    }

    function sameDate(first, second) {
        return first && second
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }

    function modelCount(value) {
        if (!value)
            return 0
        if (typeof value.count === "number")
            return value.count
        return typeof value.length === "number" ? value.length : 0
    }
}
