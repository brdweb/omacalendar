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
    property string buttonText: "Color"
    readonly property bool paletteVisible: palettePopup.opened
    signal colorSelected(string colorValue)

    readonly property var colorChoices: [
        "#7aa2f7", "#bb9af7", "#f7768e", "#ff9e64", "#e0af68",
        "#9ece6a", "#73daca", "#2ac3de", "#7dcfff", "#c0caf5"
    ]

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
        Accessible.name: "Choose " + root.buttonText.toLowerCase()
        onClicked: palettePopup.open()

        contentItem: RowLayout {
            spacing: 8
            Rectangle {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                radius: 6
                color: root.selectedColor
                border.color: Theme.alpha(Theme.text, 0.32)
            }
            Text {
                Layout.fillWidth: true
                text: root.buttonText
                color: Theme.text
                font.pixelSize: Theme.smallFontSize
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                text: "⌄"
                color: Theme.mutedText
                font.pixelSize: Theme.fontSize
            }
        }

        background: Rectangle {
            radius: Theme.smallRadius
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
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        padding: 0
                        hoverEnabled: true
                        Accessible.name: "Choose calendar color " + modelData
                        onClicked: {
                            root.selectedColor = modelData
                            root.colorSelected(modelData)
                            palettePopup.close()
                        }
                        contentItem: Item {}
                        background: Rectangle {
                            radius: 8
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
                    text: "Provider color"
                    compact: true
                    quiet: true
                    onClicked: {
                        root.colorSelected("")
                        palettePopup.close()
                    }
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    text: "Custom…"
                    compact: true
                    onClicked: {
                        palettePopup.close()
                        customColorDialog.open()
                    }
                }
            }
        }

        background: Rectangle {
            radius: Theme.radius
            color: Theme.surface
            border.color: Theme.border
        }
    }

    ColorDialog {
        id: customColorDialog
        title: "Choose calendar color"
        selectedColor: root.selectedColor
        onAccepted: {
            root.selectedColor = selectedColor
            root.colorSelected(String(selectedColor))
        }
    }
}
