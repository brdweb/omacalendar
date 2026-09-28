pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Calendar visibility, colours, order, calendar sets and local calendars.
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

        RowLayout {
            Layout.fillWidth: true
            SectionLabel { Layout.fillWidth: true; text: qsTr("CALENDAR SETS") }
            AppButton {
                iconText: "+"
                text: qsTr("New set")
                compact: true
                onClicked: tab.drawer.calendarSetDialogRef.openNew()
            }
        }

        Repeater {
            model: tab.drawer.effectiveCalendarSetsModel
            delegate: Rectangle {
                id: setCard
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: setRow.implicitHeight + 20
                radius: Theme.radiusLG
                color: Theme.background
                border.color: Theme.border
                RowLayout {
                    id: setRow
                    anchors.fill: parent
                    anchors.margins: Theme.spacingSM
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: setCard.modelData.name || qsTr("Calendar set")
                            color: Theme.text
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            textFormat: Text.PlainText
                            text: (setCard.modelData.calendarIds || []).length
                                  + qsTr(" calendar(s)")
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                        }
                    }
                    StatusBadge {
                        visible: setCard.modelData.isDefault === true
                        text: qsTr("Built in")
                        tone: "neutral"
                    }
                    AppButton {
                        visible: setCard.modelData.isDefault !== true
                        text: qsTr("Edit")
                        compact: true
                        quiet: true
                        onClicked: tab.drawer.calendarSetDialogRef.openExisting(
                                       setCard.modelData)
                    }
                    AppButton {
                        visible: setCard.modelData.isDefault !== true
                        iconText: "×"
                        compact: true
                        quiet: true
                        destructive: true
                        toolTipText: qsTr("Remove calendar set")
                        onClicked: tab.drawer.removeCalendarSetRequested(
                                       String(setCard.modelData.id))
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            SectionLabel { Layout.fillWidth: true; text: qsTr("CALENDARS") }
            AppButton {
                iconText: "+"
                text: qsTr("New local")
                compact: true
                onClicked: tab.drawer.localCalendarDialogRef.open()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: defaultCalendarRow.implicitHeight + 28
            radius: Theme.radiusLG
            color: Theme.surfaceAlt
            border.color: Theme.border

            RowLayout {
                id: defaultCalendarRow
                anchors.fill: parent
                anchors.margins: Theme.spacingMD
                spacing: Theme.spacingLG

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: qsTr("Default calendar")
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: qsTr("Used automatically for new events in the app and widget.")
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                        wrapMode: Text.Wrap
                    }
                }

                AppComboBox {
                    id: defaultCalendarSelector
                    objectName: "defaultCalendarSelector"
                    Layout.preferredWidth: 220
                    model: tab.drawer.defaultCalendarOptions
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: tab.drawer.defaultCalendarIndex()
                    enabled: count > 0
                    Accessible.name: qsTr("Default calendar for new events")
                    onActivated: index => {
                        const option = tab.drawer.defaultCalendarOptions[index]
                        if (option)
                            tab.drawer.preferenceChanged("defaultCalendarId",
                                                   option.value)
                    }
                }
            }
        }

        Repeater {
            model: tab.drawer.effectiveCalendarsModel
            delegate: Rectangle {
                id: calendarCard
                required property var modelData
                objectName: "calendarCard-" + String(modelData.id || "")
                readonly property string calendarId: String(
                                                          modelData.id
                                                          || "")
                readonly property bool deleting:
                    tab.drawer.deletingCalendarId.length > 0
                    && tab.drawer.deletingCalendarId === calendarId
                readonly property int orderIndex:
                    tab.drawer.calendarOrderIndex(calendarId)
                Layout.fillWidth: true
                implicitHeight: calendarSettings.implicitHeight + 24
                radius: Theme.radiusLG
                z: reorderMouse.drag.active ? 100
                   : calendarDropArea.validDrop ? 50 : 0
                color: calendarDropArea.validDrop
                       ? Theme.alpha(Theme.accent, 0.12)
                       : Theme.background
                border.width: calendarDropArea.validDrop ? 2 : 1
                border.color: calendarDropArea.validDrop
                              ? Theme.accent : Theme.border
                enabled: !deleting
                opacity: reorderMouse.drag.active ? 0.68
                                                  : deleting ? 0.5 : 1
                Behavior on opacity {
                    NumberAnimation { duration: 120 }
                }

                DropArea {
                    id: calendarDropArea
                    objectName: "calendarDropArea-"
                                + calendarCard.calendarId
                    anchors.fill: parent
                    property bool placeAfter: false
                    function sourceCalendarId(source) {
                        if (!source)
                            return ""
                        const prefix = "calendarCard-"
                        const sourceName = String(source.objectName || "")
                        return sourceName.startsWith(prefix)
                                ? sourceName.slice(prefix.length) : ""
                    }
                    readonly property string draggedCalendarId:
                        sourceCalendarId(drag.source)
                    readonly property bool validDrop:
                        containsDrag && draggedCalendarId
                        && draggedCalendarId !== calendarCard.calendarId
                    onEntered: drag => placeAfter = drag.y > height / 2
                    onPositionChanged: drag =>
                                           placeAfter = drag.y > height / 2
                    onDropped: drop => {
                        const sourceId = sourceCalendarId(drop.source)
                        if (sourceId)
                            tab.drawer.reorderCalendar(sourceId,
                                        calendarCard.calendarId,
                                        placeAfter)
                        drop.acceptProposedAction()
                    }
                }

                Rectangle {
                    z: 12
                    objectName: "calendarDropIndicator-"
                                + calendarCard.calendarId
                    visible: calendarDropArea.validDrop
                    x: 6
                    y: calendarDropArea.placeAfter
                       ? calendarCard.height - height / 2 : -height / 2
                    width: calendarCard.width - 12
                    height: 28
                    radius: Theme.radiusMD
                    color: Theme.accent

                    Text {
                        textFormat: Text.PlainText
                        anchors.centerIn: parent
                        width: parent.width - 20
                        text: qsTr("Drop ")
                              + (calendarDropArea.placeAfter
                                 ? qsTr("after ") : qsTr("before "))
                              + String(calendarCard.modelData.name
                                       || qsTr("calendar"))
                        color: Theme.accentText
                        font.pixelSize: Theme.smallFontSize
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }

                ColumnLayout {
                    id: calendarSettings
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMD
                    spacing: Theme.spacingXS
                    RowLayout {
                        Layout.fillWidth: true

                        Item {
                            id: dragHandle
                            objectName: "calendarDragHandle-"
                                        + calendarCard.calendarId
                            Layout.preferredWidth: 30
                            Layout.preferredHeight: 30
                            Accessible.name: qsTr("Drag to reorder ")
                                             + String(calendarCard.modelData.name
                                                      || qsTr("calendar"))
                            Accessible.role: Accessible.Button

                            Text {
                                textFormat: Text.PlainText
                                z: 1
                                anchors.centerIn: parent
                                text: Theme.glyphDragHandle
                                color: reorderMouse.drag.active
                                       ? Theme.accent
                                       : reorderMouse.containsMouse
                                       ? Theme.text : Theme.mutedText
                                font.pixelSize: Theme.iconFontSize
                                font.weight: Font.DemiBold
                            }
                            MouseArea {
                                id: reorderMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: pressed
                                             ? Qt.ClosedHandCursor
                                             : Qt.OpenHandCursor
                                drag.target: dragProxy
                                onReleased: {
                                    dragProxy.Drag.drop()
                                    dragProxy.x = 0
                                    dragProxy.y = 0
                                }
                            }
                            Item {
                                id: dragProxy
                                objectName: "calendarDragPreview-"
                                            + calendarCard.calendarId
                                z: 100
                                width: 210
                                height: 38
                                visible: reorderMouse.drag.active
                                Drag.active: reorderMouse.drag.active
                                Drag.source: calendarCard
                                Drag.supportedActions: Qt.MoveAction
                                Drag.hotSpot.x: 18
                                Drag.hotSpot.y: height / 2

                                Rectangle {
                                    anchors.fill: parent
                                    radius: Theme.radiusMD
                                    color: Theme.surface
                                    border.width: 2
                                    border.color: Theme.accent

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.spacingSM
                                        anchors.rightMargin: Theme.spacingMD
                                        spacing: Theme.spacingSM
                                        Rectangle {
                                            Layout.preferredWidth: 10
                                            Layout.preferredHeight: 10
                                            radius: width / 2
                                            color: calendarCard.modelData.colorOverride
                                                   || calendarCard.modelData.color
                                                   || Theme.accent
                                        }
                                        Text {
                                            textFormat: Text.PlainText
                                            Layout.fillWidth: true
                                            text: calendarCard.modelData.name
                                                  || qsTr("Calendar")
                                            color: Theme.text
                                            font.pixelSize: Theme.smallFontSize
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            textFormat: Text.PlainText
                                            text: qsTr("Move")
                                            color: Theme.accent
                                            font.pixelSize: Theme.microFontSize
                                            font.weight: Font.DemiBold
                                        }
                                    }
                                }
                            }
                        }

                        // Keyboard-reachable equivalent of the drag
                        // handle, committing the same positions.
                        AppButton {
                            objectName: "calendarMoveUp-"
                                        + calendarCard.calendarId
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            iconText: Theme.glyphUp
                            compact: true
                            quiet: true
                            enabled: calendarCard.orderIndex > 0
                            toolTipText: qsTr("Move calendar up")
                            Accessible.name: qsTr("Move ")
                                + String(calendarCard.modelData.name
                                         || qsTr("calendar"))
                                + qsTr(" up")
                            onClicked: tab.drawer.moveCalendarByOffset(
                                           calendarCard.calendarId, -1)
                        }
                        AppButton {
                            objectName: "calendarMoveDown-"
                                        + calendarCard.calendarId
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            iconText: Theme.glyphDown
                            compact: true
                            quiet: true
                            enabled: calendarCard.orderIndex >= 0
                                     && calendarCard.orderIndex
                                        < tab.drawer.calendars.length - 1
                            toolTipText: qsTr("Move calendar down")
                            Accessible.name: qsTr("Move ")
                                + String(calendarCard.modelData.name
                                         || qsTr("calendar"))
                                + qsTr(" down")
                            onClicked: tab.drawer.moveCalendarByOffset(
                                           calendarCard.calendarId, 1)
                        }

                        Rectangle {
                            Layout.preferredWidth: 10
                            Layout.preferredHeight: 10
                            radius: width / 2
                            color: calendarCard.modelData.colorOverride
                                   || calendarCard.modelData.color
                                   || Theme.accent
                        }
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: calendarCard.modelData.name || qsTr("Calendar")
                            color: Theme.text
                            font.pixelSize: Theme.fontSize
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            objectName: "calendarDeleting-"
                                        + calendarCard.calendarId
                            textFormat: Text.PlainText
                            visible: calendarCard.deleting
                            text: qsTr("Deleting…")
                            color: Theme.mutedText
                            font.pixelSize: Theme.smallFontSize
                        }
                        StatusBadge {
                            visible: calendarCard.modelData.readOnly === true
                            text: qsTr("Read only")
                            tone: "neutral"
                        }
                        StatusBadge {
                            objectName: "calendarProvider-"
                                        + String(calendarCard.modelData.id || "")
                            visible: calendarCard.modelData.id === "local-default"
                            text: qsTr("On this device · Built in")
                            tone: "neutral"
                        }
                        AppButton {
                            objectName: "deleteLocalCalendar-"
                                        + String(calendarCard.modelData.id || "")
                            visible: tab.drawer.calendarCanBeDeleted(
                                         calendarCard.modelData)
                            text: qsTr("Delete")
                            compact: true
                            quiet: true
                            destructive: true
                            toolTipText: qsTr("Permanently delete calendar and its events")
                            onClicked: tab.drawer.localCalendarRemoveConfirmRef.openFor(
                                           calendarCard.modelData)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingMD
                        AppCheckBox {
                            text: qsTr("Visible")
                            checked: calendarCard.modelData.visible !== false
                                     && calendarCard.modelData.enabled !== false
                            onToggled: tab.drawer.calendarPreferenceChanged(
                                           calendarCard.modelData.id,
                                           "visible", checked)
                        }
                        AppCheckBox {
                            id: muteInvitationAlerts
                            objectName: "muteInvitationAlerts-"
                                        + calendarCard.calendarId
                            text: qsTr("Mute alerts")
                            checked: calendarCard.modelData.ignoreAlerts === true
                            ToolTip.visible: hovered
                                                 && !reorderMouse.drag.active
                            ToolTip.delay: 450
                            ToolTip.text: qsTr("Suppresses desktop notifications for reminders and for new, changed, or cancelled invitations on this calendar.")
                            Accessible.description: ToolTip.text
                            onToggled: tab.drawer.calendarPreferenceChanged(
                                           calendarCard.modelData.id,
                                           "ignoreAlerts", checked)
                        }
                        Item { Layout.fillWidth: true }
                        AppColorPicker {
                            objectName: "calendarColorPicker-"
                                        + calendarCard.calendarId
                            Layout.preferredWidth: 132
                            selectedColor: calendarCard.modelData.colorOverride
                                           || calendarCard.modelData.color
                                           || Theme.accent
                            allowClear: true
                            onColorSelected: colorValue =>
                                                 tab.drawer.calendarPreferenceChanged(
                                                     calendarCard.modelData.id,
                                                     "colorOverride",
                                                     colorValue)
                        }
                    }
                }
            }
        }
        Item { Layout.preferredHeight: 12 }
    }
}
