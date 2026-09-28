pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../TaskGroups.js" as TaskGroups

// Creates or edits one task.
Dialog {
    id: taskEditor

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 48 : 460)
    padding: Theme.spacingLG
    closePolicy: Popup.CloseOnEscape
    title: editing ? qsTr("Edit task") : qsTr("New task")

    property var taskLists: []
    property var taskData: ({})
    readonly property bool editing: Boolean(taskData && taskData.id)
    readonly property var writableLists: taskLists.filter(function(list) {
        return list.readOnly !== true
    })
    readonly property var sourceList: {
        for (let index = 0; index < taskLists.length; ++index)
            if (String(taskLists[index].id) === String(taskData.listId || ""))
                return taskLists[index]
        return ({})
    }
    readonly property bool readOnly: editing && sourceList.readOnly === true
    property bool deleteArmed: false
    property string validationError: ""

    signal saveRequested(var task)
    signal removeRequested(string taskId)

    function openNew(listId, dueDate) {
        taskData = ({"listId": listId || ""})
        titleField.text = ""
        notesField.text = ""
        dueField.text = dueDate || ""
        doneBox.checked = false
        listBox.currentIndex = Math.max(0, listIndex(listId))
        deleteArmed = false
        validationError = ""
        open()
        titleField.forceActiveFocus()
    }

    function openExisting(task) {
        taskData = task || ({})
        titleField.text = String(taskData.title || "")
        notesField.text = String(taskData.notes || "")
        dueField.text = String(taskData.dueDate || "")
        doneBox.checked = taskData.completed === true
        listBox.currentIndex = Math.max(0, listIndex(taskData.listId))
        deleteArmed = false
        validationError = ""
        open()
        titleField.forceActiveFocus()
    }

    function listIndex(listId) {
        for (let index = 0; index < writableLists.length; ++index)
            if (String(writableLists[index].id) === String(listId || ""))
                return index
        return -1
    }

    function shiftedDay(days) {
        const now = new Date()
        return TaskGroups.dayKey(new Date(now.getFullYear(), now.getMonth(),
                                          now.getDate() + days))
    }

    function validate() {
        if (titleField.text.trim().length === 0)
            return qsTr("Give the task a title.")
        const due = dueField.text.trim()
        if (due.length > 0) {
            const parsed = new Date(due + "T00:00:00")
            if (!/^\d{4}-\d{2}-\d{2}$/.test(due) || isNaN(parsed.getTime())
                    || TaskGroups.dayKey(parsed) !== due)
                return qsTr("Enter the due date as YYYY-MM-DD, or leave it empty.")
        }
        if (!editing && writableLists.length === 0)
            return qsTr("No task list can be written to.")
        return ""
    }

    function submit() {
        validationError = validate()
        if (validationError.length > 0)
            return
        const task = {
            "title": titleField.text.trim(),
            "notes": notesField.text,
            "dueDate": dueField.text.trim(),
            "completed": doneBox.checked
        }
        if (editing) {
            task.id = String(taskData.id)
            task.localRevision = taskData.localRevision
        } else {
            task.listId = String(writableLists[listBox.currentIndex].id)
        }
        taskEditor.saveRequested(task)
        taskEditor.close()
    }

    function requestDelete() {
        if (!deleteArmed) {
            deleteArmed = true
            return
        }
        taskEditor.removeRequested(String(taskData.id))
        taskEditor.close()
    }

    background: Rectangle {
        radius: Theme.radiusLG
        color: Theme.surface
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMD

        AppTextField {
            id: titleField
            objectName: "taskTitle"
            Layout.fillWidth: true
            placeholderText: qsTr("Title")
            accessibleName: qsTr("Task title")
            enabled: !taskEditor.readOnly
            onAccepted: taskEditor.submit()
        }
        TextArea {
            id: notesField
            objectName: "taskNotes"
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            placeholderText: qsTr("Notes")
            color: Theme.text
            placeholderTextColor: Theme.mutedText
            enabled: !taskEditor.readOnly
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            Accessible.name: qsTr("Task notes")
            background: Rectangle {
                radius: Theme.radiusMD
                color: Theme.background
                border.color: notesField.activeFocus ? Theme.focus : Theme.border
            }
        }
        RowLayout {
            Layout.fillWidth: true
            AppTextField {
                id: dueField
                objectName: "taskDue"
                Layout.fillWidth: true
                placeholderText: qsTr("Due YYYY-MM-DD")
                accessibleName: qsTr("Due date")
                enabled: !taskEditor.readOnly
            }
            AppButton {
                text: qsTr("Today")
                compact: true
                quiet: true
                enabled: !taskEditor.readOnly
                onClicked: dueField.text = taskEditor.shiftedDay(0)
            }
            AppButton {
                text: qsTr("Tomorrow")
                compact: true
                quiet: true
                enabled: !taskEditor.readOnly
                onClicked: dueField.text = taskEditor.shiftedDay(1)
            }
            AppButton {
                text: qsTr("Clear")
                compact: true
                quiet: true
                enabled: !taskEditor.readOnly && dueField.text.length > 0
                onClicked: dueField.text = ""
            }
        }
        AppComboBox {
            id: listBox
            objectName: "taskListBox"
            Layout.fillWidth: true
            visible: !taskEditor.editing && taskEditor.writableLists.length > 1
            model: taskEditor.writableLists.map(function(list) {
                return String(list.name || "")
            })
            Accessible.name: qsTr("Task list")
        }
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: taskEditor.editing
            text: qsTr("In %1").arg(String(taskEditor.sourceList.name || ""))
                  + (taskEditor.readOnly ? "  " + qsTr("(read-only)") : "")
            color: Theme.mutedText
            font.pixelSize: Theme.smallFontSize
        }
        AppCheckBox {
            id: doneBox
            objectName: "taskDone"
            text: qsTr("Done")
            enabled: !taskEditor.readOnly
        }
        Text {
            textFormat: Text.PlainText
            objectName: "taskValidation"
            Layout.fillWidth: true
            visible: text.length > 0
            text: taskEditor.validationError
            color: Theme.warning
            wrapMode: Text.Wrap
            font.pixelSize: Theme.smallFontSize
        }
        RowLayout {
            Layout.fillWidth: true
            AppButton {
                objectName: "taskDelete"
                visible: taskEditor.editing && !taskEditor.readOnly
                text: taskEditor.deleteArmed ? qsTr("Confirm delete") : qsTr("Delete")
                quiet: !taskEditor.deleteArmed
                onClicked: taskEditor.requestDelete()
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                quiet: true
                onClicked: taskEditor.close()
            }
            AppButton {
                objectName: "taskSave"
                text: qsTr("Save")
                primary: true
                enabled: !taskEditor.readOnly
                onClicked: taskEditor.submit()
            }
        }
    }
}
