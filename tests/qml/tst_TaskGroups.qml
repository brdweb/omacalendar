import QtQuick
import QtTest
import "../../src/app/qml/TaskGroups.js" as TaskGroups

TestCase {
    name: "TaskGroups"

    readonly property var lists: [
        {"id": "home", "name": "Home", "color": "#9ece6a"},
        {"id": "work", "name": "Work", "color": "#7aa2f7"},
        {"id": "hidden", "name": "Hidden", "enabled": false}
    ]
    readonly property var tasks: [
        {"id": "late", "listId": "home", "title": "Pay rent", "dueDate": "2026-09-27"},
        {"id": "now", "listId": "work", "title": "Send report", "dueDate": "2026-09-28"},
        {"id": "soon-b", "listId": "home", "title": "B", "dueDate": "2026-10-02"},
        {"id": "soon-a", "listId": "home", "title": "A", "dueDate": "2026-10-02"},
        {"id": "first", "listId": "work", "title": "First", "dueDate": "2026-09-30"},
        {"id": "whenever", "listId": "home", "title": "Read"},
        {"id": "done", "listId": "home", "title": "Done", "dueDate": "2026-09-01",
         "completed": true},
        {"id": "secret", "listId": "hidden", "title": "Hidden", "dueDate": "2026-09-01"}
    ]

    function taskIds(rows) {
        return rows.filter(function(row) { return row.kind === "task" })
                   .map(function(row) { return row.task.id })
    }

    function test_day_key_pads() {
        compare(TaskGroups.dayKey(new Date(2026, 0, 5)), "2026-01-05")
    }

    function test_groups_in_order_without_hidden_lists() {
        const rows = TaskGroups.rows(tasks, lists, "2026-09-28", "", false)
        compare(rows.filter(function(row) { return row.kind === "header" })
                    .map(function(row) { return row.key }),
                ["overdue", "today", "upcoming", "undated"])
        compare(taskIds(rows), ["late", "now", "first", "soon-a", "soon-b", "whenever"],
                "due day, then title; hidden lists and completed tasks left out")
        compare(rows[1].color, "#9ece6a", "each task carries its list's color")
    }

    function test_completed_and_one_list() {
        const rows = TaskGroups.rows(tasks, lists, "2026-09-28", "home", true)
        compare(taskIds(rows), ["late", "soon-a", "soon-b", "whenever", "done"])
        compare(rows[rows.length - 2].key, "completed")
        compare(taskIds(TaskGroups.rows(tasks, lists, "2026-09-28", "hidden", false)),
                ["secret"], "a hidden list still shows when chosen")
    }

    function test_read_only_lists() {
        const readOnlyLists = [{"id": "home"}, {"id": "shared", "readOnly": true}]
        verify(TaskGroups.isReadOnly(readOnlyLists, "shared"))
        verify(!TaskGroups.isReadOnly(readOnlyLists, "home"))
        const rows = TaskGroups.rows([{"id": "t", "listId": "shared", "title": "x"}],
                                     readOnlyLists, "2026-09-28", "", false)
        compare(rows[1].readOnly, true)
    }

    function test_due_counts() {
        compare(TaskGroups.dueCount(tasks, lists, "2026-09-28"), 2,
                "overdue and today, open, in visible lists")
        compare(TaskGroups.dueOn(tasks, lists, "2026-10-02").map(function(task) {
            return task.id
        }), ["soon-a", "soon-b"])
    }
}
