pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../components"
import "../EventIndex.js" as EventIndex
import "../TaskGroups.js" as TaskGroups

Item {
    id: root

    property date currentDate: new Date()
    property var events: []
    // Built once per events change; day cells look their events up here.
    readonly property var eventIndex: EventIndex.build(events)
    property string selectedEventReference: ""
    property int dayCount: 31
    property string timeFormat: "system"
    // Open tasks are listed on the day they are due.
    property var tasks: []
    property var taskLists: []
    signal eventActivated(var eventData)
    signal taskActivated(var task)
    signal taskCompletionRequested(string taskId, bool completed)
    signal createRequested(date dateValue)
    signal dateSelected(date dateValue)
    // Scrolled to the last loaded day; the owner may raise dayCount and load
    // the matching range.
    signal moreDaysRequested()

    // Only the days on screen exist as items, so a long agenda stays cheap.
    ListView {
        id: agendaList
        objectName: "agendaList"
        anchors.fill: parent
        clip: true
        spacing: Theme.spacingXS
        boundsBehavior: Flickable.StopAtBounds
        model: root.dayCount
        ScrollBar.vertical: ScrollBar {}
        footer: Item { width: agendaList.width; height: 22 }
        onAtYEndChanged: {
            if (atYEnd && count > 0 && contentHeight > height)
                root.moreDaysRequested()
        }

        delegate: ColumnLayout {
            id: daySection
            required property int index
            readonly property date dateValue: new Date(root.currentDate.getFullYear(),
                                                        root.currentDate.getMonth(),
                                                        root.currentDate.getDate() + index)
            readonly property var dayEvents: root.eventsForDate(dateValue)
            readonly property var dayTasks: TaskGroups.dueOn(root.tasks, root.taskLists,
                                                             TaskGroups.dayKey(dateValue))
            width: agendaList.width
            spacing: Theme.spacingSM

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: daySection.index === 0 ? 0 : 13
                spacing: Theme.spacingSM

                Rectangle {
                    Layout.preferredWidth: 42
                    Layout.preferredHeight: 42
                    radius: Theme.radiusMD
                    activeFocusOnTab: true
                    Accessible.name: Qt.formatDate(daySection.dateValue,
                                                   "dddd, MMMM d")
                    Accessible.role: Accessible.Button
                    color: root.sameDate(daySection.dateValue, new Date())
                           ? Theme.accent : Theme.surface
                    border.color: activeFocus
                                  || root.sameDate(daySection.dateValue,
                                                   root.currentDate)
                                  ? Theme.focus : Theme.border
                    border.width: activeFocus ? 2 : 1
                    Keys.onReturnPressed: event => {
                        root.dateSelected(daySection.dateValue)
                        event.accepted = true
                    }
                    Keys.onEnterPressed: event => {
                        root.dateSelected(daySection.dateValue)
                        event.accepted = true
                    }
                    Column {
                        anchors.centerIn: parent
                        spacing: -2
                        Text {
                            textFormat: Text.PlainText
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: Qt.formatDate(daySection.dateValue, "ddd").toUpperCase()
                            color: root.sameDate(daySection.dateValue, new Date())
                                   ? Theme.accentText : Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                            font.weight: Font.DemiBold
                        }
                        Text {
                            textFormat: Text.PlainText
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: daySection.dateValue.getDate()
                            color: root.sameDate(daySection.dateValue, new Date())
                                   ? Theme.accentText : Theme.text
                            font.pixelSize: Theme.fontSize + 2
                            font.weight: Font.Bold
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.dateSelected(daySection.dateValue)
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        textFormat: Text.PlainText
                        text: Qt.formatDate(daySection.dateValue, "dddd")
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                        font.weight: Font.DemiBold
                    }
                    Text {
                        textFormat: Text.PlainText
                        text: Qt.formatDate(daySection.dateValue, "MMMM d")
                        color: Theme.mutedText
                        font.pixelSize: Theme.smallFontSize
                    }
                }

                AppButton {
                    iconText: "+"
                    quiet: true
                    compact: true
                    toolTipText: qsTr("New event on ")
                                 + Qt.formatDate(daySection.dateValue, "MMMM d")
                    onClicked: root.createRequested(daySection.dateValue)
                }
            }

            Repeater {
                model: daySection.dayEvents
                delegate: EventRow {
                    required property var modelData
                    Layout.fillWidth: true
                    eventData: modelData
                    selected: root.eventReference(modelData)
                              === root.selectedEventReference
                    timeFormat: root.timeFormat
                    continuationText: root.continuationLabel(modelData,
                                                             daySection.dateValue)
                    onEditRequested: value => root.eventActivated(value)
                }
            }

            Repeater {
                model: daySection.dayTasks
                delegate: ItemDelegate {
                    id: taskRow
                    required property var modelData
                    objectName: "agendaTask-" + modelData.id
                    Layout.fillWidth: true
                    Layout.leftMargin: 50
                    implicitHeight: 34
                    Accessible.name: qsTr("Task due: %1").arg(String(modelData.title || ""))
                    onClicked: root.taskActivated(modelData)
                    background: Rectangle {
                        radius: Theme.radiusMD
                        color: taskRow.hovered ? Theme.alpha(Theme.text, 0.045)
                                               : "transparent"
                    }
                    contentItem: RowLayout {
                        spacing: Theme.spacingSM
                        AppCheckBox {
                            checked: false
                            Accessible.name: qsTr("Mark %1 done").arg(
                                                 String(taskRow.modelData.title || ""))
                            onToggled: root.taskCompletionRequested(
                                           String(taskRow.modelData.id), checked)
                        }
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: String(taskRow.modelData.title || "")
                            color: Theme.text
                            font.pixelSize: Theme.smallFontSize
                            elide: Text.ElideRight
                        }
                        Text {
                            textFormat: Text.PlainText
                            text: qsTr("Task")
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                        }
                    }
                }
            }

            Rectangle {
                visible: daySection.dayEvents.length === 0 && daySection.dayTasks.length === 0
                Layout.fillWidth: true
                implicitHeight: 44
                radius: Theme.radiusMD
                color: dayEmptyMouse.containsMouse
                       ? Theme.alpha(Theme.text, 0.045) : "transparent"
                Text {
                    textFormat: Text.PlainText
                    anchors.left: parent.left
                    anchors.leftMargin: 54
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("No events")
                    color: Theme.alpha(Theme.mutedText, 0.68)
                    font.pixelSize: Theme.smallFontSize
                }
                MouseArea {
                    id: dayEmptyMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onDoubleClicked: root.createRequested(daySection.dateValue)
                }
            }
        }
    }

    function sameDate(first, second) {
        return first && second
                && first.getFullYear() === second.getFullYear()
                && first.getMonth() === second.getMonth()
                && first.getDate() === second.getDate()
    }

    function eventStart(value) {
        return EventIndex.eventStart(value)
    }

    function eventEnd(value) {
        return EventIndex.eventEnd(value)
    }

    function eventsForDate(dateValue) {
        return EventIndex.eventsForDate(eventIndex, dateValue)
    }

    function continuationLabel(value, dateValue) {
        const start = eventStart(value)
        const end = eventEnd(value)
        const dayStart = new Date(dateValue.getFullYear(), dateValue.getMonth(),
                                  dateValue.getDate())
        const dayEnd = new Date(dayStart.getFullYear(), dayStart.getMonth(),
                                dayStart.getDate() + 1)
        if (start < dayStart && end > dayEnd)
            return qsTr("Continues")
        if (start < dayStart)
            return qsTr("Ends today")
        if (end > dayEnd)
            return qsTr("Continues tomorrow")
        return ""
    }

    function eventReference(value) {
        return String(value.id || "") + "\n" + String(value.recurrenceId || "")
    }
}
