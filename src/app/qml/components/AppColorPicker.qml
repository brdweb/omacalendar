pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import OmaCalendar

Item {
    id: root

    property color selectedColor: Theme.accent
    property bool allowClear: false
    property string buttonText: qsTr("Color")
    property string clearText: qsTr("Provider color")
    // %1 is the colour name announced for each swatch.
    property string swatchNameFormat: qsTr("%1 calendar color")
    property string dialogTitle: qsTr("Choose calendar color")
    // False when the owner drives selectedColor from its own state, so a
    // choice does not break that binding.
    property bool selectionFollowsChoice: true
    readonly property bool paletteVisible: palettePopup.opened
    signal colorSelected(string colorValue)

    // Follows the active theme by default; see Theme.calendarPalette.
    property var colorChoices: Theme.calendarPalette

    implicitWidth: 132
    implicitHeight: Theme.controlHeight

    function openPalette() {
        palettePopup.open()
    }

    function closePalette() {
        palettePopup.close()
    }

    Button {
        id: colorButton
        anchors.fill: parent
        hoverEnabled: true
        Accessible.name: qsTr("Choose %1").arg(root.buttonText.toLowerCase())
        onClicked: palettePopup.open()

        contentItem: RowLayout {
            spacing: 8
            Rectangle {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                radius: Theme.radiusSM
                color: root.selectedColor
                border.color: Theme.alpha(Theme.text, 0.32)
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.buttonText
                color: Theme.text
                font.pixelSize: Theme.smallFontSize
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                textFormat: Text.PlainText
                text: "⌄"
                color: Theme.mutedText
                font.pixelSize: Theme.fontSize
            }
        }

        background: Rectangle {
            radius: Theme.radiusMD
            color: colorButton.down || palettePopup.opened
                   ? Theme.surfaceAlt : Theme.surface
            border.width: colorButton.activeFocus ? 2 : 1
            border.color: colorButton.activeFocus ? Theme.focus : Theme.border
        }
    }

    Popup {
        id: palettePopup
        x: root.width - width
        y: root.height + 5
        width: 272
        padding: 12
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        contentItem: ColumnLayout {
            spacing: 10

            GridLayout {
                Layout.fillWidth: true
                columns: 5
                columnSpacing: 8
                rowSpacing: 8

                Repeater {
                    model: root.colorChoices
                    delegate: Button {
                        id: swatch
                        required property string modelData
                        required property int index
                        objectName: "colorSwatch-" + index
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        padding: 0
                        hoverEnabled: true
                        // Announce a colour name; the hex value stays as the
                        // description for users who want the exact colour.
                        Accessible.name: root.swatchNameFormat
                                         .arg(Theme.colorName(swatch.modelData))
                        Accessible.description: swatch.modelData
                        onClicked: {
                            if (root.selectionFollowsChoice)
                                root.selectedColor = modelData
                            root.colorSelected(modelData)
                            palettePopup.close()
                        }
                        contentItem: Item {}
                        background: Rectangle {
                            radius: Theme.radiusMD
                            color: swatch.modelData
                            border.width: String(root.selectedColor).toLowerCase()
                                          === swatch.modelData.toLowerCase() ? 3 : 1
                            border.color: String(root.selectedColor).toLowerCase()
                                          === swatch.modelData.toLowerCase()
                                          ? Theme.text
                                          : Theme.alpha(Theme.text, 0.28)
                        }
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
                spacing: 8
                AppButton {
                    visible: root.allowClear
                    objectName: "colorClear"
                    text: root.clearText
                    compact: true
                    quiet: true
                    onClicked: {
                        root.colorSelected("")
                        palettePopup.close()
                    }
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Custom…")
                    compact: true
                    onClicked: {
                        palettePopup.close()
                        customColorDialog.open()
                    }
                }
            }
        }

        background: Rectangle {
            radius: Theme.radiusLG
            color: Theme.surface
            border.color: Theme.border
        }
    }

    ColorDialog {
        id: customColorDialog
        title: root.dialogTitle
        selectedColor: root.selectedColor
        onAccepted: {
            if (root.selectionFollowsChoice)
                root.selectedColor = selectedColor
            root.colorSelected(String(selectedColor))
        }
    }
}
