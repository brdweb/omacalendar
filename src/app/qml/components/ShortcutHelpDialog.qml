pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Lists the keyboard shortcuts, grouped by section.
Dialog {
    id: helpDialog

    property var catalog: []

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, Overlay.overlay ? Overlay.overlay.width - 48 : 560)
    height: Math.min(640, Overlay.overlay ? Overlay.overlay.height - 48 : 640)
    padding: Theme.spacingLG
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    title: qsTr("Keyboard shortcuts")

    background: Rectangle {
        radius: Theme.radiusLG
        color: Theme.surface
        border.color: Theme.border
    }

    header: RowLayout {
        spacing: Theme.spacingSM
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            Layout.margins: Theme.spacingLG
            text: helpDialog.title
            color: Theme.text
            font.pixelSize: Theme.titleFontSize
            font.bold: true
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        AppCloseButton {
            Layout.rightMargin: Theme.spacingMD
            toolTipText: qsTr("Close keyboard shortcuts")
            onClicked: helpDialog.close()
        }
    }

    contentItem: ScrollView {
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            objectName: "shortcutHelpList"
            width: helpDialog.availableWidth
            spacing: 2

            Repeater {
                model: helpDialog.catalog
                delegate: ColumnLayout {
                    id: entry
                    required property var modelData
                    required property int index
                    readonly property bool startsSection: index === 0
                            || helpDialog.catalog[index - 1].section !== modelData.section
                    Layout.fillWidth: true
                    spacing: 2

                    Text {
                        visible: entry.startsSection
                        textFormat: Text.PlainText
                        Layout.topMargin: entry.index === 0 ? 0 : Theme.spacingMD
                        text: String(entry.modelData.section)
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                        font.bold: true
                        Accessible.role: Accessible.Heading
                        Accessible.name: text
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingMD
                        Accessible.role: Accessible.ListItem
                        Accessible.name: String(entry.modelData.action) + ": "
                                         + String(entry.modelData.keys)
                        Text {
                            textFormat: Text.PlainText
                            Layout.preferredWidth: Math.round(helpDialog.availableWidth * 0.4)
                            text: String(entry.modelData.keys)
                            color: Theme.accent
                            font.family: "monospace"
                            wrapMode: Text.Wrap
                        }
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: String(entry.modelData.action)
                            color: Theme.text
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
    }
}
