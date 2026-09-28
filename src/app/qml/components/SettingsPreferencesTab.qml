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
                    Layout.fillWidth: true
                    model: [qsTr("System week start"), qsTr("Monday"), qsTr("Sunday")]
                    currentIndex: Math.max(0, [0, 1, 7].indexOf(
                                               Number(tab.drawer.preferences.firstDayOfWeek
                                                      || 0)))
                    Accessible.name: qsTr("First day of week")
                    onActivated: index => tab.drawer.preferenceChanged(
                                     "firstDayOfWeek", [0, 1, 7][index])
                }
                AppCheckBox {
                    objectName: "showWeekNumbers"
                    text: qsTr("Show week numbers")
                    checked: tab.drawer.preferences.showWeekNumbers === true
                             || tab.drawer.preferences.showWeekNumbers === "true"
                    onToggled: tab.drawer.preferenceChanged("showWeekNumbers", checked)
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
        Item { Layout.preferredHeight: 12 }
    }
}
