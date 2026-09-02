pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

ColumnLayout {
    id: root

    default property alias contentData: body.data
    property string title: ""
    property bool expanded: true
    property string detail: ""

    Layout.fillWidth: true
    spacing: 8

    Button {
        id: headerButton
        objectName: "accordion-" + root.title.toLowerCase().replace(/[^a-z0-9]+/g, "-")
        Layout.fillWidth: true
        implicitHeight: 44
        padding: 0
        hoverEnabled: true
        Accessible.name: (root.expanded ? qsTr("Collapse ") : qsTr("Expand ")) + root.title
        onClicked: root.expanded = !root.expanded

        contentItem: RowLayout {
            spacing: 10
            Text {
                text: root.expanded ? "⌄" : "›"
                color: Theme.accent
                font.pixelSize: Theme.fontSize + 3
                Layout.preferredWidth: 18
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: root.title
                color: Theme.text
                font.pixelSize: Theme.fontSize
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                visible: root.detail.length > 0
                text: root.detail
                color: Theme.mutedText
                font.pixelSize: Theme.smallFontSize
            }
        }

        background: Rectangle {
            radius: Theme.smallRadius
            color: headerButton.hovered ? Theme.alpha(Theme.text, 0.055)
                                        : Theme.darkBackground
            border.color: Theme.border
        }
    }

    ColumnLayout {
        id: body
        visible: root.expanded
        Layout.fillWidth: true
        spacing: 10
    }
}
