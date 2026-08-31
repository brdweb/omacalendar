pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import OmaCalendar

ComboBox {
    id: control

    implicitWidth: 190
    implicitHeight: Theme.controlHeight
    leftPadding: 12
    rightPadding: 36
    topPadding: 0
    bottomPadding: 0
    font.pixelSize: Theme.fontSize
    hoverEnabled: true

    delegate: ItemDelegate {
        id: optionDelegate
        required property int index
        required property var modelData
        width: control.width - 8
        implicitHeight: 38
        highlighted: control.highlightedIndex === index
        hoverEnabled: true
        leftPadding: 10
        rightPadding: 10
        contentItem: Text {
            text: control.textRole.length > 0 && optionDelegate.modelData
                  && optionDelegate.modelData[control.textRole] !== undefined
                  ? String(optionDelegate.modelData[control.textRole])
                  : String(optionDelegate.modelData === undefined
                           ? "" : optionDelegate.modelData)
            color: optionDelegate.highlighted ? Theme.text : Theme.mutedText
            font.pixelSize: Theme.fontSize
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Theme.smallRadius
            color: optionDelegate.highlighted || optionDelegate.hovered
                   ? Theme.accentSoft : "transparent"
        }
    }

    contentItem: Text {
        leftPadding: 0
        rightPadding: 0
        text: control.displayText
        color: control.enabled ? Theme.text : Theme.alpha(Theme.mutedText, 0.65)
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.width - width - 12
        y: Math.round((control.height - height) / 2)
        text: "⌄"
        color: control.enabled ? Theme.mutedText : Theme.alpha(Theme.mutedText, 0.55)
        font.pixelSize: Theme.fontSize + 2
    }

    background: Rectangle {
        radius: Theme.smallRadius
        color: control.down || control.popup.visible ? Theme.surfaceAlt : Theme.surface
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.focus : Theme.border
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                                 320)
        topPadding: 4
        bottomPadding: 4
        leftPadding: 4
        rightPadding: 4
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            radius: Theme.smallRadius
            color: Theme.surface
            border.color: Theme.border
        }
    }
}
