pragma ComponentBehavior: Bound
import QtQuick
import OmaCalendar

// Accepts .ics files dragged in from a file manager. It only answers
// text/uri-list drags, so the views' own event drags (key
// "omacalendar-event") pass through untouched. The controller makes the final
// check through the same local-file validation as command-line imports.
DropArea {
    id: root

    // True while an acceptable file hovers over the window.
    readonly property bool hovering: containsDrag && pendingAcceptable

    property bool pendingAcceptable: false

    signal importRequested(url fileUrl)

    keys: ["text/uri-list"]

    // The first local .ics URL in a list, or "" when there is none.
    function firstIcsUrl(urls) {
        const list = urls || []
        for (let index = 0; index < list.length; ++index) {
            const text = String(list[index])
            if (/^file:/i.test(text) && /\.ics$/i.test(text.split(/[?#]/)[0]))
                return text
        }
        return ""
    }

    function handleEntered(drag) {
        pendingAcceptable = firstIcsUrl(drag.urls).length > 0
        drag.accepted = pendingAcceptable
    }

    function handleDropped(drop) {
        const fileUrl = firstIcsUrl(drop.urls)
        pendingAcceptable = false
        if (fileUrl.length === 0) {
            drop.accepted = false
            return false
        }
        drop.accepted = true
        root.importRequested(fileUrl)
        return true
    }

    onEntered: drag => handleEntered(drag)
    onExited: pendingAcceptable = false
    onDropped: drop => handleDropped(drop)

    Rectangle {
        objectName: "icsDropHighlight"
        anchors.fill: parent
        anchors.margins: Theme.spacingSM
        visible: root.hovering
        radius: Theme.radiusLG
        color: Theme.alpha(Theme.accent, 0.08)
        border.color: Theme.accent
        border.width: 2

        Text {
            textFormat: Text.PlainText
            anchors.centerIn: parent
            text: qsTr("Drop to import the .ics file")
            color: Theme.text
            font.pixelSize: Theme.fontSize + 2
            font.weight: Font.DemiBold
        }
    }
}
