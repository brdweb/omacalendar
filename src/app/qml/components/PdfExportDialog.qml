pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Chooses what to print: a date range, a layout and how much detail. The
// owner asks for a destination file and saves the PDF.
Dialog {
    id: pdfExportDialog
    anchors.centerIn: Overlay.overlay
    width: Math.min(520, Overlay.overlay ? Overlay.overlay.width - 48 : 520)
    modal: true
    title: qsTr("Save as PDF")
    standardButtons: Dialog.Cancel

    property string validationError: ""

    signal optionsChosen(var options)

    // range is {first, last} as Date values; layout is "list" or "month".
    function openFor(range, layout) {
        validationError = ""
        firstDateField.text = Qt.formatDate(range.first, "yyyy-MM-dd")
        lastDateField.text = Qt.formatDate(range.last, "yyyy-MM-dd")
        layoutBox.currentIndex = layout === "month" ? 1 : 0
        open()
    }

    function chosenOptions() {
        const first = new Date(firstDateField.text + "T00:00:00")
        const last = new Date(lastDateField.text + "T00:00:00")
        if (!/^\d{4}-\d{2}-\d{2}$/.test(firstDateField.text)
                || !/^\d{4}-\d{2}-\d{2}$/.test(lastDateField.text)
                || isNaN(first.getTime()) || isNaN(last.getTime())) {
            validationError = qsTr("Enter dates as YYYY-MM-DD.")
            return null
        }
        if (last < first) {
            validationError = qsTr("The last day comes before the first.")
            return null
        }
        if ((last - first) / 86400000 > 366) {
            validationError = qsTr("Print at most one year at a time.")
            return null
        }
        validationError = ""
        return {
            "firstDate": firstDateField.text,
            "lastDate": lastDateField.text,
            "layout": layoutBox.currentIndex === 1 ? "month" : "list",
            "includeDetails": detailsBox.checked,
            "visibleOnly": visibleOnlyBox.checked
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMD
        RowLayout {
            Layout.fillWidth: true
            AppTextField {
                id: firstDateField
                objectName: "pdfFirstDate"
                Layout.fillWidth: true
                placeholderText: qsTr("YYYY-MM-DD")
                accessibleName: qsTr("First day to print")
            }
            Text { textFormat: Text.PlainText; text: qsTr("to"); color: Theme.mutedText }
            AppTextField {
                id: lastDateField
                objectName: "pdfLastDate"
                Layout.fillWidth: true
                placeholderText: qsTr("YYYY-MM-DD")
                accessibleName: qsTr("Last day to print")
            }
        }
        AppComboBox {
            id: layoutBox
            objectName: "pdfLayout"
            Layout.fillWidth: true
            model: [qsTr("List of events by day"), qsTr("Month grid")]
            Accessible.name: qsTr("Layout")
        }
        AppCheckBox {
            id: detailsBox
            objectName: "pdfIncludeDetails"
            enabled: layoutBox.currentIndex === 0
            text: qsTr("Include location, calendar and notes")
        }
        AppCheckBox {
            id: visibleOnlyBox
            objectName: "pdfVisibleOnly"
            checked: true
            text: qsTr("Only calendars shown now")
        }
        Text {
            textFormat: Text.PlainText
            visible: text.length > 0
            Layout.fillWidth: true
            text: pdfExportDialog.validationError
            color: Theme.warning
            wrapMode: Text.Wrap
            font.pixelSize: Theme.smallFontSize
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "pdfChooseDestination"
                text: qsTr("Choose destination…")
                primary: true
                onClicked: {
                    const options = pdfExportDialog.chosenOptions()
                    if (options !== null)
                        pdfExportDialog.optionsChosen(options)
                }
            }
        }
    }
}
