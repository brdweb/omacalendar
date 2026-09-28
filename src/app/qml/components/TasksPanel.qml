pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../TaskGroups.js" as TaskGroups

// To-dos from every task list, grouped by when they are due.
Drawer {
    id: root

    property var tasks: []
    property var taskLists: []
    // The owner keeps this current (it ticks with the app clock), so "Today"
    // stays right overnight while the panel is open.
    property date today: new Date()
    readonly property string todayKey: TaskGroups.dayKey(today)
    readonly property var writableLists: taskLists.filter(function(list) {
        return list.readOnly !== true
    })
    property string filterListId: ""
    property bool showCompleted: false
    readonly property var rows: TaskGroups.rows(tasks, taskLists, todayKey, filterListId,
                                                showCompleted)

    signal createRequested(var task)
    signal completionRequested(string taskId, bool completed)
    signal editRequested(var task)

    edge: Qt.RightEdge
    width: Math.min(Theme.panelWidth, Overlay.overlay
                    ? Overlay.overlay.width * 0.42 : Theme.panelWidth)
    height: Overlay.overlay ? Overlay.overlay.height : 720
    modal: false
    dim: false

    onOpened: addField.forceActiveFocus()

    function groupTitle(key) {
        if (key === "overdue")
            return qsTr("Overdue")
        if (key === "today")
            return qsTr("Today")
        if (key === "upcoming")
            return qsTr("Upcoming")
        if (key === "undated")
            return qsTr("No date")
        return qsTr("Completed")
    }

    function dueText(task) {
        const due = String(task.dueDate || "")
        if (!due)
            return ""
        if (due === todayKey)
            return qsTr("Today")
        const tomorrow = new Date(today.getFullYear(), today.getMonth(),
                                  today.getDate() + 1)
        if (due === TaskGroups.dayKey(tomorrow))
            return qsTr("Tomorrow")
        return Qt.formatDate(new Date(due + "T00:00:00"), "ddd d MMM")
    }

    // The list new tasks go into: the one shown, else the first writable one.
    function targetListId() {
        if (filterListId)
            return filterListId
        return writableLists.length > 0 ? String(writableLists[0].id) : ""
    }

    function addTask() {
        const title = addField.text.trim()
        if (title.length === 0)
            return
        const task = {"title": title}
        const listId = targetListId()
        if (listId)
            task.listId = listId
        root.createRequested(task)
        addField.text = ""
    }

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingSM

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingLG
            Layout.bottomMargin: 0

            AppCloseButton {
                toolTipText: qsTr("Close tasks")
                onClicked: root.close()
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: qsTr("Tasks")
                color: Theme.text
                font.pixelSize: Theme.fontSize + 4
                font.weight: Font.Bold
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLG
            Layout.rightMargin: Theme.spacingLG
            AppComboBox {
                id: listBox
                objectName: "taskListFilter"
                Layout.fillWidth: true
                model: [qsTr("All lists")].concat(root.taskLists.map(function(list) {
                    return String(list.name || "")
                }))
                Accessible.name: qsTr("Task list")
                onActivated: index => root.filterListId = index <= 0
                             ? "" : String(root.taskLists[index - 1].id)
            }
            AppCheckBox {
                id: completedBox
                objectName: "showCompletedTasks"
                text: qsTr("Completed")
                checked: root.showCompleted
                onToggled: root.showCompleted = checked
            }
        }

        AppTextField {
            id: addField
            objectName: "addTaskField"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLG
            Layout.rightMargin: Theme.spacingLG
            placeholderText: qsTr("Add a task")
            accessibleName: qsTr("New task title")
            enabled: root.writableLists.length > 0
            onAccepted: root.addTask()
        }

        ListView {
            id: taskView
            objectName: "taskList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.rows
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Loader {
                id: rowLoader
                required property var modelData
                width: ListView.view.width
                sourceComponent: modelData.kind === "header" ? headerRow : taskRow

                Component {
                    id: headerRow
                    Text {
                        textFormat: Text.PlainText
                        leftPadding: Theme.spacingLG
                        topPadding: Theme.spacingMD
                        bottomPadding: Theme.spacingXS
                        text: root.groupTitle(rowLoader.modelData.key) + "  "
                              + rowLoader.modelData.count
                        color: rowLoader.modelData.key === "overdue" ? Theme.danger
                                                                     : Theme.mutedText
                        font.pixelSize: Theme.microFontSize
                        font.weight: Font.DemiBold
                        font.capitalization: Font.AllUppercase
                    }
                }

                Component {
                    id: taskRow
                    ItemDelegate {
                        id: delegateItem
                        readonly property var task: rowLoader.modelData.task
                        implicitHeight: Math.max(40, content.implicitHeight + 12)
                        Accessible.name: String(task.title || "")
                        onClicked: root.editRequested(task)
                        background: Rectangle {
                            color: delegateItem.hovered ? Theme.surfaceAlt : "transparent"
                        }
                        contentItem: RowLayout {
                            id: content
                            spacing: Theme.spacingSM
                            AppCheckBox {
                                objectName: "taskDone-" + delegateItem.task.id
                                checked: delegateItem.task.completed === true
                                enabled: rowLoader.modelData.readOnly !== true
                                Accessible.name: checked
                                                 ? qsTr("Mark %1 not done")
                                                   .arg(delegateItem.task.title)
                                                 : qsTr("Mark %1 done")
                                                   .arg(delegateItem.task.title)
                                onToggled: root.completionRequested(
                                               String(delegateItem.task.id), checked)
                            }
                            Rectangle {
                                visible: rowLoader.modelData.color.length > 0
                                         && root.taskLists.length > 1
                                Layout.preferredWidth: 6
                                Layout.preferredHeight: 6
                                radius: 3
                                color: rowLoader.modelData.color || Theme.accent
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: String(delegateItem.task.title || "")
                                    color: delegateItem.task.completed ? Theme.mutedText
                                                                       : Theme.text
                                    font.strikeout: delegateItem.task.completed === true
                                    font.pixelSize: Theme.smallFontSize
                                    elide: Text.ElideRight
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: String(delegateItem.task.notes || "")
                                          .split("\n")[0]
                                    color: Theme.mutedText
                                    font.pixelSize: Theme.microFontSize
                                    elide: Text.ElideRight
                                }
                            }
                            Text {
                                textFormat: Text.PlainText
                                visible: text.length > 0
                                text: root.dueText(delegateItem.task)
                                color: rowLoader.modelData.group === "overdue"
                                       ? Theme.danger : Theme.mutedText
                                font.pixelSize: Theme.microFontSize
                            }
                        }
                    }
                }
            }

            EmptyState {
                anchors.centerIn: parent
                width: parent.width - Theme.spacingXL * 2
                visible: taskView.count === 0
                title: qsTr("Nothing to do")
                description: root.writableLists.length > 0
                         ? qsTr("Add a task above.")
                         : qsTr("No task list can be written to.")
            }
        }
    }
}
