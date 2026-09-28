pragma ComponentBehavior: Bound
// App is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Previews an iCalendar file and imports it into a writable calendar.
Dialog {
    id: icsImportDialog
    property url fileUrl
    property var preview: ({})
    property var writableCalendars: []
    anchors.centerIn: Overlay.overlay
    width: Math.min(580, Overlay.overlay ? Overlay.overlay.width - 48 : 580)
    modal: true
    title: qsTr("Import iCalendar events")
    standardButtons: Dialog.Cancel

    function localFileName(fileUrl) {
        const value = String(fileUrl || "")
        const slash = value.lastIndexOf("/")
        return slash >= 0 ? value.slice(slash + 1) : value
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMD
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: icsImportDialog.localFileName(icsImportDialog.fileUrl)
            color: Theme.text
            font.weight: Font.DemiBold
            elide: Text.ElideMiddle
        }
        AppComboBox {
            id: importCalendarBox
            Layout.fillWidth: true
            model: icsImportDialog.writableCalendars
            textRole: "name"
            valueRole: "id"
            Accessible.name: qsTr("Import destination calendar")
        }
        AppComboBox {
            id: duplicatePolicyBox
            Layout.fillWidth: true
            model: [
                {"text": qsTr("Skip matching UIDs"), "value": "skip"},
                {"text": qsTr("Import duplicates as copies"), "value": "copy"},
                {"text": qsTr("Replace matching UIDs"), "value": "replace"}
            ]
            textRole: "text"
            valueRole: "value"
            Accessible.name: qsTr("Duplicate import handling")
        }
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: icsImportDialog.preview.count === undefined
                  ? qsTr("Preview the file before importing.")
                  : Number(icsImportDialog.preview.count) + qsTr(" event(s), ")
                    + Number(icsImportDialog.preview.duplicateCount || 0)
                    + qsTr(" matching UID(s)")
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
        }
        ColumnLayout {
            Layout.fillWidth: true
            Repeater {
                model: (icsImportDialog.preview.events || []).slice(0, 6)
                delegate: Text {
                    textFormat: Text.PlainText
                    required property var modelData
                    Layout.fillWidth: true
                    text: "• " + (modelData.event.summary || qsTr("Untitled event"))
                          + (modelData.duplicate ? qsTr("  ·  duplicate") : "")
                    color: modelData.duplicate ? Theme.warning : Theme.text
                    font.pixelSize: Theme.smallFontSize
                    elide: Text.ElideRight
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Preview")
                enabled: importCalendarBox.currentIndex >= 0
                onClicked: App.previewIcsImport(icsImportDialog.fileUrl,
                                                importCalendarBox.currentValue)
            }
            AppButton {
                text: qsTr("Import")
                primary: true
                enabled: Number(icsImportDialog.preview.count || 0) > 0
                onClicked: App.commitIcsImport(icsImportDialog.fileUrl,
                                               importCalendarBox.currentValue,
                                               duplicatePolicyBox.currentValue)
            }
        }
    }
}
