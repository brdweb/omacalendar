import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "components"

ApplicationWindow {
    id: window
    width: 1280
    height: 820
    minimumWidth: 920
    minimumHeight: 620
    visible: true
    title: "OmaCalendar"
    color: Theme.background

    property date visibleMonth: new Date(App.selectedDate.getFullYear(), App.selectedDate.getMonth(), 1)

    function sameDate(first, second) {
        return first && second
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }

    function eventDate(eventData) {
        return eventData.allDay ? new Date(eventData.startDate + "T00:00:00")
                                : new Date(eventData.startUtc)
    }

    function eventsFor(date) {
        const result = []
        for (let index = 0; index < App.events.length; ++index) {
            const value = App.events[index]
            if (sameDate(eventDate(value), date)) result.push(value)
        }
        return result
    }

    function loadVisibleMonth() {
        const start = new Date(visibleMonth.getFullYear(), visibleMonth.getMonth(), -7)
        const end = new Date(visibleMonth.getFullYear(), visibleMonth.getMonth() + 1, 14)
        App.loadRange(start, end)
    }

    header: Rectangle {
        height: 66
        color: Theme.darkBackground
        border.color: Theme.alpha(Theme.text, 0.08)
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 12
            Text {
                text: "oma"
                color: Theme.mutedText
                font.pixelSize: Theme.fontSize + 4
                font.weight: Font.Light
            }
            Text {
                text: "calendar"
                color: Theme.text
                font.pixelSize: Theme.fontSize + 4
                font.weight: Font.Bold
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                width: connectionLabel.implicitWidth + 24
                height: 30
                radius: 15
                color: Theme.alpha(App.connected ? Theme.success : Theme.danger, 0.13)
                Row {
                    anchors.centerIn: parent
                    spacing: 7
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7; height: 7; radius: 4
                        color: App.connected ? Theme.success : Theme.danger
                    }
                    Text {
                        id: connectionLabel
                        text: App.connected ? "Local service" : "Offline"
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                    }
                }
            }
            AppButton {
                text: "+  New event"
                primary: true
                onClicked: editor.openNew(App.selectedDate)
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 278
            Layout.fillHeight: true
            color: Theme.darkBackground
            border.color: Theme.alpha(Theme.text, 0.08)

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 14
                RowLayout {
                    Layout.fillWidth: true
                    AppButton {
                        text: "‹"
                        quiet: true
                        Layout.preferredWidth: 38
                        onClicked: {
                            visibleMonth = new Date(visibleMonth.getFullYear(), visibleMonth.getMonth() - 1, 1)
                            loadVisibleMonth()
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: Qt.formatDate(visibleMonth, "MMMM yyyy")
                        color: Theme.text
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: Theme.fontSize
                        font.weight: Font.DemiBold
                    }
                    AppButton {
                        text: "›"
                        quiet: true
                        Layout.preferredWidth: 38
                        onClicked: {
                            visibleMonth = new Date(visibleMonth.getFullYear(), visibleMonth.getMonth() + 1, 1)
                            loadVisibleMonth()
                        }
                    }
                }
                DayOfWeekRow {
                    Layout.fillWidth: true
                    locale: Qt.locale()
                    delegate: Text {
                        text: shortName
                        color: Theme.mutedText
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: Theme.smallFontSize - 1
                    }
                }
                MonthGrid {
                    id: monthGrid
                    Layout.fillWidth: true
                    Layout.preferredHeight: 224
                    month: visibleMonth.getMonth()
                    year: visibleMonth.getFullYear()
                    locale: Qt.locale()
                    delegate: MonthCell {
                        required property var model
                        date: model.date
                        month: monthGrid.month
                        selected: window.sameDate(model.date, App.selectedDate)
                        eventCount: window.eventsFor(model.date).length
                        onClicked: App.setSelectedDate(model.date)
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Theme.alpha(Theme.text, 0.08)
                }
                SectionLabel { text: "CALENDARS" }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: App.calendars
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        height: 38
                        background: Rectangle {
                            color: parent.hovered ? Theme.alpha(Theme.text, 0.06) : "transparent"
                            radius: 8
                        }
                        contentItem: RowLayout {
                            Rectangle {
                                width: 9; height: 9; radius: 5
                                color: modelData.color || Theme.accent
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.name
                                color: Theme.text
                                elide: Text.ElideRight
                                font.pixelSize: Theme.smallFontSize
                            }
                            Text {
                                text: modelData.readOnly ? "view" : ""
                                color: Theme.mutedText
                                font.pixelSize: Theme.smallFontSize - 1
                            }
                        }
                    }
                }
                AppButton {
                    Layout.fillWidth: true
                    text: "Accounts & settings"
                    onClicked: settingsDrawer.open()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.background

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 28
                spacing: 18
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        spacing: 2
                        Text {
                            text: Qt.formatDate(App.selectedDate, "dddd")
                            color: Theme.mutedText
                            font.pixelSize: Theme.fontSize
                        }
                        Text {
                            text: Qt.formatDate(App.selectedDate, "MMMM d")
                            color: Theme.text
                            font.pixelSize: Theme.titleFontSize
                            font.weight: Font.Bold
                        }
                    }
                    Item { Layout.fillWidth: true }
                    AppButton {
                        text: "Today"
                        onClicked: {
                            App.setSelectedDate(new Date())
                            visibleMonth = new Date()
                            loadVisibleMonth()
                        }
                    }
                }

                Rectangle {
                    visible: App.lastError.length > 0
                    Layout.fillWidth: true
                    implicitHeight: errorText.implicitHeight + 22
                    radius: Theme.radius
                    color: Theme.alpha(Theme.danger, 0.12)
                    border.color: Theme.alpha(Theme.danger, 0.4)
                    Text {
                        id: errorText
                        anchors.fill: parent
                        anchors.margins: 11
                        text: App.lastError
                        color: Theme.text
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.smallFontSize
                    }
                }

                ListView {
                    id: agenda
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 10
                    model: window.eventsFor(App.selectedDate)
                    delegate: EventRow {
                        required property var modelData
                        eventData: modelData
                        onEditRequested: value => editor.openExisting(value)
                    }
                    ScrollBar.vertical: ScrollBar {}
                    Text {
                        anchors.centerIn: parent
                        visible: agenda.count === 0
                        text: App.calendars.length === 0
                              ? "Add a calendar account to get started"
                              : "Nothing scheduled — enjoy the breathing room."
                        color: Theme.mutedText
                        font.pixelSize: Theme.fontSize
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: App.busy ? "Working…" : App.statusText
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                        elide: Text.ElideRight
                    }
                    Text {
                        text: "Theme: " + OmarchyTheme.sourceName
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                    }
                }
            }
        }
    }

    EventEditor {
        id: editor
        onSaveRequested: value => value.id ? App.updateEvent(value) : App.createEvent(value)
        onRemoveRequested: eventId => App.removeEvent(eventId)
    }

    Drawer {
        id: settingsDrawer
        edge: Qt.RightEdge
        width: Math.min(440, window.width * 0.42)
        height: window.height
        background: Rectangle { color: Theme.surface }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 26
            spacing: 16
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "Accounts"
                    color: Theme.text
                    font.pixelSize: Theme.titleFontSize
                    font.weight: Font.Bold
                }
                AppButton { text: "Close"; quiet: true; onClicked: settingsDrawer.close() }
            }
            Text {
                Layout.fillWidth: true
                text: "Google and CalDAV connections are configured here. Credentials are stored in your desktop keyring, never in the calendar database."
                color: Theme.mutedText
                wrapMode: Text.Wrap
                font.pixelSize: Theme.smallFontSize
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: parent.width
                    spacing: 14

                    RowLayout {
                        Layout.fillWidth: true
                        SectionLabel {
                            Layout.fillWidth: true
                            text: "CONNECTED ACCOUNTS"
                        }
                        AppButton {
                            text: "Sync now"
                            quiet: true
                            enabled: App.connected && App.accounts.length > 0 && !App.busy
                            onClicked: App.syncAll()
                        }
                    }

                    Repeater {
                        model: App.accounts
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 82
                            radius: Theme.radius
                            color: Theme.background
                            border.color: Theme.alpha(Theme.text, 0.1)
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 13
                                spacing: 10
                                Rectangle {
                                    width: 10
                                    height: 10
                                    radius: 5
                                    color: modelData.authStatus === "connected"
                                           ? Theme.success : Theme.accent
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.displayName || modelData.principal || "Calendar account"
                                        color: Theme.text
                                        elide: Text.ElideRight
                                        font.pixelSize: Theme.fontSize
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.provider.toUpperCase() + "  ·  " + modelData.authStatus.replace(/_/g, " ")
                                        color: Theme.mutedText
                                        elide: Text.ElideRight
                                        font.pixelSize: Theme.smallFontSize
                                    }
                                }
                                AppButton {
                                    text: "Remove"
                                    quiet: true
                                    Layout.preferredWidth: 76
                                    onClicked: App.removeAccount(modelData.id)
                                }
                            }
                        }
                    }

                    Rectangle {
                        visible: App.accounts.length === 0
                        Layout.fillWidth: true
                        implicitHeight: emptyAccounts.implicitHeight + 30
                        radius: Theme.radius
                        color: Theme.background
                        border.color: Theme.alpha(Theme.text, 0.1)
                        Text {
                            id: emptyAccounts
                            anchors.fill: parent
                            anchors.margins: 15
                            text: "No accounts yet. Connect Google or a CalDAV server below."
                            color: Theme.mutedText
                            wrapMode: Text.Wrap
                            font.pixelSize: Theme.smallFontSize
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Theme.alpha(Theme.text, 0.08)
                    }

                    SectionLabel { text: "GOOGLE CALENDAR" }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: googleCard.implicitHeight + 30
                        radius: Theme.radius
                        color: Theme.background
                        border.color: Theme.alpha(Theme.text, 0.1)
                        ColumnLayout {
                            id: googleCard
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 15
                            spacing: 10
                            Text {
                                Layout.fillWidth: true
                                text: "Sign in securely in your browser. OmaCalendar requests access only to your calendar list and events."
                                color: Theme.mutedText
                                wrapMode: Text.Wrap
                                font.pixelSize: Theme.smallFontSize
                            }
                            AppTextField {
                                id: googleDisplayName
                                Layout.fillWidth: true
                                placeholderText: "Account label (optional)"
                            }
                            AppButton {
                                Layout.fillWidth: true
                                text: "Connect Google"
                                primary: true
                                enabled: App.connected && !App.busy
                                onClicked: App.connectGoogle(googleDisplayName.text.trim())
                            }
                        }
                    }

                    SectionLabel { text: "CALDAV" }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: caldavCard.implicitHeight + 30
                        radius: Theme.radius
                        color: Theme.background
                        border.color: Theme.alpha(Theme.text, 0.1)
                        ColumnLayout {
                            id: caldavCard
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 15
                            spacing: 10
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
                                id: caldavUsername
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
                                enabled: App.connected && !App.busy
                                         && caldavEndpoint.text.trim().length > 0
                                         && caldavUsername.text.trim().length > 0
                                         && caldavPassword.text.length > 0
                                onClicked: {
                                    App.addCalDavAccount(caldavEndpoint.text,
                                                         caldavUsername.text,
                                                         caldavPassword.text,
                                                         caldavName.text)
                                    caldavPassword.text = ""
                                }
                            }
                        }
                    }

                    Item { Layout.preferredHeight: 8 }
                }
            }
        }
    }
}
