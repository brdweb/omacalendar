pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Display, time zone, default calendar and notification preferences.
ScrollView {
    id: tab
    required property var drawer

    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ColumnLayout {
        width: tab.drawer.width - 40
        x: 20
        spacing: Theme.spacingMD
        Item { Layout.preferredHeight: 5 }

        SectionLabel { text: qsTr("DISPLAY") }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: displayPreferences.implicitHeight + 24
            radius: Theme.radiusLG
            color: Theme.background
            border.color: Theme.border
            ColumnLayout {
                id: displayPreferences
                anchors.fill: parent
                anchors.margins: Theme.spacingMD
                spacing: Theme.spacingSM
                AppComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("System time format"), qsTr("12-hour"), qsTr("24-hour")]
                    currentIndex: Math.max(0, ["system", "12h", "24h"].indexOf(
                                               String(tab.drawer.preferences.timeFormat
                                                      || "system")))
                    Accessible.name: qsTr("Time format")
                    onActivated: index => tab.drawer.preferenceChanged(
                                     "timeFormat", ["system", "12h", "24h"][index])
                }
                AppComboBox {
                    objectName: "firstDayOfWeek"
                    Layout.fillWidth: true
                    model: [qsTr("System week start"), qsTr("Monday"), qsTr("Sunday"),
                            qsTr("Saturday")]
                    currentIndex: Math.max(0, [0, 1, 7, 6].indexOf(
                                               Number(tab.drawer.preferences.firstDayOfWeek
                                                      || 0)))
                    Accessible.name: qsTr("First day of week")
                    onActivated: index => tab.drawer.preferenceChanged(
                                     "firstDayOfWeek", [0, 1, 7, 6][index])
                }
                AppComboBox {
                    objectName: "weekViewDays"
                    Layout.fillWidth: true
                    model: [
                        {"text": qsTr("Week view: whole week"), "value": 7},
                        {"text": qsTr("Week view: 6 days"), "value": 6},
                        {"text": qsTr("Week view: 5 days"), "value": 5},
                        {"text": qsTr("Week view: 4 days"), "value": 4},
                        {"text": qsTr("Week view: 3 days"), "value": 3},
                        {"text": qsTr("Week view: 2 days"), "value": 2}
                    ]
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: Math.max(0, [7, 6, 5, 4, 3, 2].indexOf(
                                               Number(tab.drawer.preferences.weekViewDays
                                                      || 7)))
                    Accessible.name: qsTr("Days in the week view")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Fewer than seven days start at the selected date")
                    onActivated: tab.drawer.preferenceChanged("weekViewDays", currentValue)
                }
                AppCheckBox {
                    objectName: "showWeekNumbers"
                    text: qsTr("Show week numbers")
                    checked: tab.drawer.preferences.showWeekNumbers === true
                             || tab.drawer.preferences.showWeekNumbers === "true"
                    onToggled: tab.drawer.preferenceChanged("showWeekNumbers", checked)
                }
                // Stored inverted (hideWeekends, hideDeclined) so an unset
                // key keeps today's behaviour.
                AppCheckBox {
                    objectName: "showWeekends"
                    text: qsTr("Show weekends in week and month views")
                    checked: !(tab.drawer.preferences.hideWeekends === true
                               || tab.drawer.preferences.hideWeekends === "true")
                    onToggled: tab.drawer.preferenceChanged("hideWeekends", !checked)
                }
                AppCheckBox {
                    objectName: "showDeclined"
                    text: qsTr("Show events I declined")
                    checked: !(tab.drawer.preferences.hideDeclined === true
                               || tab.drawer.preferences.hideDeclined === "true")
                    onToggled: tab.drawer.preferenceChanged("hideDeclined", !checked)
                }
                AppComboBox {
                    id: displayTimeZoneBox
                    objectName: "displayTimeZone"
                    Layout.fillWidth: true
                    model: tab.drawer.timeZoneOptions
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: tab.drawer.displayTimeZoneIndex()
                    Accessible.name: qsTr("Display time zone")
                    onActivated: tab.drawer.preferenceChanged("displayTimeZone",
                                                        currentValue)
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingSM
                    AppComboBox {
                        id: secondaryTimeZoneBox
                        objectName: "secondaryTimeZone"
                        Layout.fillWidth: true
                        model: tab.drawer.secondaryTimeZoneOptions
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: tab.drawer.secondaryTimeZoneIndex()
                        Accessible.name: qsTr("Second time zone in day and week views")
                        onActivated: tab.drawer.preferenceChanged("secondaryTimeZone",
                                                                  currentValue)
                    }
                    AppButton {
                        objectName: "swapTimeZones"
                        enabled: String(tab.drawer.preferences.secondaryTimeZone || "").length > 0
                        text: qsTr("Swap")
                        toolTipText: qsTr("Make the second time zone the display time zone")
                        onClicked: tab.drawer.swapTimeZones()
                    }
                }
                AppComboBox {
                    Layout.fillWidth: true
                    model: [
                        {"text": qsTr("Default duration: 15 minutes"), "value": 15},
                        {"text": qsTr("Default duration: 30 minutes"), "value": 30},
                        {"text": qsTr("Default duration: 45 minutes"), "value": 45},
                        {"text": qsTr("Default duration: 1 hour"), "value": 60},
                        {"text": qsTr("Default duration: 90 minutes"), "value": 90},
                        {"text": qsTr("Default duration: 2 hours"), "value": 120}
                    ]
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: Math.max(0, [15, 30, 45, 60, 90, 120].indexOf(
                                               Number(tab.drawer.preferences.defaultDuration
                                                      || 60)))
                    Accessible.name: qsTr("Default event duration")
                    onActivated: tab.drawer.preferenceChanged("defaultDuration",
                                                        currentValue)
                }
                AppComboBox {
                    objectName: "defaultReminderMinutes"
                    Layout.fillWidth: true
                    model: tab.drawer.reminderOptions(
                               qsTr("Timed event reminder: "),
                               tab.drawer.preferences.defaultReminderMinutes, "")
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: tab.drawer.reminderOptionIndex(
                                      model, tab.drawer.preferences.defaultReminderMinutes, 15)
                    Accessible.name: qsTr("Default reminder for new timed events")
                    onActivated: tab.drawer.preferenceChanged("defaultReminderMinutes",
                                                              currentValue)
                }
                AppComboBox {
                    objectName: "defaultAllDayReminderMinutes"
                    Layout.fillWidth: true
                    model: tab.drawer.reminderOptions(
                               qsTr("All-day event reminder: "),
                               tab.drawer.preferences.defaultAllDayReminderMinutes, "")
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: tab.drawer.reminderOptionIndex(
                                      model, tab.drawer.preferences.defaultAllDayReminderMinutes,
                                      15)
                    Accessible.name: qsTr("Default reminder for new all-day events")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("All-day reminders count back from midnight at the start of the day")
                    onActivated: tab.drawer.preferenceChanged("defaultAllDayReminderMinutes",
                                                              currentValue)
                }
                AppComboBox {
                    id: notificationPrivacyBox
                    objectName: "notificationPrivacy"
                    Layout.fillWidth: true
                    model: [
                        {"text": qsTr("Notifications: Private"),
                         "value": "generic"},
                        {"text": qsTr("Notifications: Event title"),
                         "value": "title_only"},
                        {"text": qsTr("Notifications: Full details"),
                         "value": "full_details"}
                    ]
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: Math.max(0, ["generic", "title_only",
                                               "full_details"].indexOf(
                                                  String(tab.drawer.preferences.notificationPrivacy
                                                         || "generic")))
                    Accessible.name: qsTr("Notification privacy")
                    onActivated: tab.drawer.preferenceChanged("notificationPrivacy",
                                                         currentValue)
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        textFormat: Text.PlainText
                        text: qsTr("Work hours")
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                    }
                    AppSpinBox {
                        from: 0
                        to: 23
                        value: Number(tab.drawer.preferences.workDayStart || 8)
                        Accessible.name: qsTr("Work day start hour")
                        onValueModified: tab.drawer.preferenceChanged("workDayStart",
                                                                 value)
                    }
                    Text { textFormat: Text.PlainText; text: qsTr("to"); color: Theme.mutedText }
                    AppSpinBox {
                        from: 1
                        to: 24
                        value: Number(tab.drawer.preferences.workDayEnd || 18)
                        Accessible.name: qsTr("Work day end hour")
                        onValueModified: tab.drawer.preferenceChanged("workDayEnd",
                                                                 value)
                    }
                }
            }
        }

        SectionLabel { text: qsTr("IMPORT & EXPORT") }
        RowLayout {
            Layout.fillWidth: true
            AppButton {
                Layout.fillWidth: true
                text: qsTr("Import .ics…")
                onClicked: tab.drawer.importIcsRequested()
            }
            AppButton {
                Layout.fillWidth: true
                text: qsTr("Export .ics…")
                onClicked: tab.drawer.exportIcsRequested()
            }
        }
        SectionLabel { text: qsTr("SUPPORT") }
        AppButton {
            Layout.fillWidth: true
            text: qsTr("Preview diagnostics")
            onClicked: tab.drawer.diagnosticsRequested()
        }
        AppButton {
            objectName: "backupNowButton"
            Layout.fillWidth: true
            text: qsTr("Back up now")
            enabled: tab.drawer.connected
            onClicked: tab.drawer.backupRequested()
        }
        Text {
            objectName: "backupResultText"
            Layout.fillWidth: true
            visible: tab.drawer.lastBackupPath.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
            text: qsTr("Saved to %1").arg(tab.drawer.lastBackupPath)
        }
        Text {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
            text: qsTr("Backups do not include account passwords or sign-in tokens; "
                       + "after restoring on another computer, sign in to each "
                       + "account again.")
        }
        Item { Layout.preferredHeight: 12 }
    }
}
