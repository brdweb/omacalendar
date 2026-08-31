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
    property int conflictCount: 0
    property int failedOperationCount: 0
    property bool connected: false

    signal dateSelected(date dateValue)
    signal monthChanged(date dateValue)
    signal setActivated(string setId)
    signal calendarVisibilityRequested(string calendarId, bool visible)
    signal panelRequested(string panelName)
    signal settingsRequested()

    color: Theme.darkBackground
    border.color: Theme.divider

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 17
        anchors.bottomMargin: 14
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            AppButton {
                iconText: "‹"
                quiet: true
                compact: true
                toolTipText: "Previous month"
                onClicked: {
                    root.monthDate = new Date(root.monthDate.getFullYear(),
                                              root.monthDate.getMonth() - 1, 1)
                    root.monthChanged(root.monthDate)
                }
            }
            Text {
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
                toolTipText: "Next month"
                onClicked: {
                    root.monthDate = new Date(root.monthDate.getFullYear(),
                                              root.monthDate.getMonth() + 1, 1)
                    root.monthChanged(root.monthDate)
                }
            }
        }

        DayOfWeekRow {
            Layout.fillWidth: true
            locale: Qt.locale()
            delegate: Text {
                required property string shortName
                text: shortName.slice(0, 1)
                color: Theme.mutedText
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: Theme.microFontSize
                font.weight: Font.DemiBold
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

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.divider
        }

        RowLayout {
            Layout.fillWidth: true
            SectionLabel {
                Layout.fillWidth: true
                text: "CALENDAR SETS"
            }
            AppButton {
                visible: root.modelCount(root.effectiveCalendarSetsModel) > 0
                iconText: "+"
                quiet: true
                compact: true
                toolTipText: "Manage calendar sets"
                onClicked: root.settingsRequested()
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 5

            Repeater {
                model: root.modelCount(root.effectiveCalendarSetsModel) > 0
                       ? root.effectiveCalendarSetsModel
                       : [{"id": "", "name": "All calendars"}]
                delegate: AppButton {
                    required property var modelData
                    text: modelData.name || "Calendar set"
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
                text: "CALENDARS"
            }
            Text {
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
                                  ? "Hide " : "Show ") + modelData.name
                onClicked: root.calendarVisibilityRequested(
                               modelData.id,
                               !root.calendarIsVisible(modelData.id))

                background: Rectangle {
                    radius: Theme.smallRadius
                    color: calendarDelegate.hovered || calendarDelegate.activeFocus
                           ? Theme.alpha(Theme.text, 0.065) : "transparent"
                    border.width: calendarDelegate.activeFocus ? 1 : 0
                    border.color: Theme.focus
                }
                contentItem: RowLayout {
                    spacing: 9
                    Rectangle {
                        Layout.preferredWidth: 13
                        Layout.preferredHeight: 13
                        radius: 4
                        color: root.calendarIsVisible(calendarDelegate.modelData.id)
                               ? (calendarDelegate.modelData.color || Theme.accent)
                               : "transparent"
                        border.width: 1
                        border.color: calendarDelegate.modelData.color || Theme.accent
                        Text {
                            visible: root.calendarIsVisible(calendarDelegate.modelData.id)
                            anchors.centerIn: parent
                            text: "✓"
                            color: Theme.accentText
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: calendarDelegate.modelData.name || "Calendar"
                        color: root.calendarIsVisible(calendarDelegate.modelData.id)
                               ? Theme.text : Theme.mutedText
                        elide: Text.ElideRight
                        font.pixelSize: Theme.smallFontSize
                    }
                    Text {
                        visible: calendarDelegate.modelData.readOnly === true
                        text: "Read only"
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
                title: "No calendars"
                description: "Connect an account or create a local calendar."
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
                Layout.fillWidth: true
                implicitHeight: 38
                text: "Invitations"
                icon.name: "mail-unread-symbolic"
                onClicked: root.panelRequested("invitations")
                contentItem: RowLayout {
                    Text { text: "◇"; color: Theme.mutedText }
                    Text {
                        Layout.fillWidth: true
                        text: "Invitations"
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
                    Text { text: "⚙"; color: Theme.mutedText }
                    Text {
                        Layout.fillWidth: true
                        text: "Accounts & settings"
                        color: Theme.text
                        font.pixelSize: Theme.smallFontSize
                    }
                }
            }
        }
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
