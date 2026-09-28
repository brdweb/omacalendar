pragma ComponentBehavior: Bound
// App is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Chooses what to export as iCalendar: a date range, a calendar set or a
// whole local calendar.
Dialog {
    id: icsExportDialog
    anchors.centerIn: Overlay.overlay
    width: Math.min(520, Overlay.overlay ? Overlay.overlay.width - 48 : 520)
    modal: true
    title: qsTr("Export iCalendar events")
    standardButtons: Dialog.Cancel

    property var calendarSets: []
    property var localWritableCalendars: []
    property int activeCalendarSetIndex: 0

    // The chosen scope, as entered; the owner validates it and asks for a
    // destination file.
    signal scopeChosen(int scopeIndex, string calendarSetId, string calendarId,
                       string rangeStart, string rangeEnd)

    function openForSelection() {
        exportScopeBox.currentIndex = 0
        rangeStartField.text = Qt.formatDate(App.selectedDate, "yyyy-MM-dd")
        const end = new Date(App.selectedDate.getFullYear(),
                             App.selectedDate.getMonth(),
                             App.selectedDate.getDate() + 1)
        rangeEndField.text = Qt.formatDate(end, "yyyy-MM-dd")
        open()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMD
        AppComboBox {
            id: exportScopeBox
            Layout.fillWidth: true
            model: [qsTr("Date range"), qsTr("Active calendar set"), qsTr("Entire local calendar")]
            Accessible.name: qsTr("Export scope")
        }
        AppComboBox {
            id: exportCalendarSetBox
            visible: exportScopeBox.currentIndex === 1
            Layout.fillWidth: true
            model: icsExportDialog.calendarSets
            textRole: "name"
            valueRole: "id"
            currentIndex: Math.max(0, icsExportDialog.activeCalendarSetIndex)
            Accessible.name: qsTr("Calendar set to export")
        }
        AppComboBox {
            id: exportLocalCalendarBox
            visible: exportScopeBox.currentIndex === 2
            Layout.fillWidth: true
            model: icsExportDialog.localWritableCalendars
            textRole: "name"
            valueRole: "id"
            Accessible.name: qsTr("Local calendar to export")
        }
        RowLayout {
            visible: exportScopeBox.currentIndex === 0
            Layout.fillWidth: true
            AppTextField {
                id: rangeStartField
                Layout.fillWidth: true
                placeholderText: qsTr("YYYY-MM-DD")
                accessibleName: qsTr("Export range start")
            }
            Text { textFormat: Text.PlainText; text: qsTr("to"); color: Theme.mutedText }
            AppTextField {
                id: rangeEndField
                Layout.fillWidth: true
                placeholderText: qsTr("YYYY-MM-DD")
                accessibleName: qsTr("Export range end")
            }
        }
        Text {
            textFormat: Text.PlainText
            visible: exportScopeBox.currentIndex === 2
                     && icsExportDialog.localWritableCalendars.length === 0
            Layout.fillWidth: true
            text: qsTr("Create a local calendar before exporting a whole calendar.")
            color: Theme.warning
            wrapMode: Text.Wrap
            font.pixelSize: Theme.smallFontSize
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Choose destination…")
                primary: true
                enabled: exportScopeBox.currentIndex !== 2
                         || exportLocalCalendarBox.currentIndex >= 0
                onClicked: icsExportDialog.scopeChosen(
                               exportScopeBox.currentIndex,
                               String(exportCalendarSetBox.currentValue || ""),
                               String(exportLocalCalendarBox.currentValue || ""),
                               rangeStartField.text, rangeEndField.text)
            }
        }
    }
}
