.pragma library

// Groups tasks for the Tasks panel. Dates are compared as yyyy-MM-dd text,
// which sorts the same way as the days themselves.

function dayKey(dateValue) {
    const month = String(dateValue.getMonth() + 1).padStart(2, "0")
    const day = String(dateValue.getDate()).padStart(2, "0")
    return dateValue.getFullYear() + "-" + month + "-" + day
}

// Lists a task may be shown from: the chosen list, or every enabled list.
function visibleListIds(lists, listId) {
    if (listId)
        return [listId]
    return (lists || []).filter(function(list) { return list.enabled !== false })
                        .map(function(list) { return String(list.id) })
}

function compareTasks(left, right) {
    const leftDue = String(left.dueDate || "")
    const rightDue = String(right.dueDate || "")
    if (leftDue !== rightDue) {
        if (!leftDue)
            return 1
        if (!rightDue)
            return -1
        return leftDue < rightDue ? -1 : 1
    }
    return String(left.title || "").localeCompare(String(right.title || ""))
}

// The kind of group an open task belongs to on the given day.
function groupKey(task, todayKey) {
    if (task.completed)
        return "completed"
    const due = String(task.dueDate || "")
    if (!due)
        return "undated"
    if (due < todayKey)
        return "overdue"
    return due === todayKey ? "today" : "upcoming"
}

// Rows for a ListView: {kind: "header", key, count} and {kind: "task", task,
// group, color}. Groups keep this order and empty ones are left out.
function rows(tasks, lists, todayKey, listId, showCompleted) {
    const order = ["overdue", "today", "upcoming", "undated", "completed"]
    const visible = visibleListIds(lists, listId)
    const colors = {}
    for (let index = 0; index < (lists || []).length; ++index)
        colors[String(lists[index].id)] = String(lists[index].color || "")
    const groups = {}
    for (let index = 0; index < (tasks || []).length; ++index) {
        const task = tasks[index]
        if (visible.indexOf(String(task.listId)) < 0)
            continue
        const key = groupKey(task, todayKey)
        if (key === "completed" && !showCompleted)
            continue
        if (!groups[key])
            groups[key] = []
        groups[key].push(task)
    }
    const result = []
    for (let groupIndex = 0; groupIndex < order.length; ++groupIndex) {
        const key = order[groupIndex]
        const members = groups[key]
        if (!members || members.length === 0)
            continue
        members.sort(compareTasks)
        result.push({"kind": "header", "key": key, "count": members.length})
        for (let index = 0; index < members.length; ++index)
            result.push({"kind": "task", "task": members[index], "group": key,
                         "color": colors[String(members[index].listId)] || ""})
    }
    return result
}

// Open tasks due today or earlier in the visible lists.
function dueCount(tasks, lists, todayKey) {
    const visible = visibleListIds(lists, "")
    let count = 0
    for (let index = 0; index < (tasks || []).length; ++index) {
        const task = tasks[index]
        const due = String(task.dueDate || "")
        if (!task.completed && due && due <= todayKey
                && visible.indexOf(String(task.listId)) >= 0)
            ++count
    }
    return count
}

// Open tasks due on one day in the visible lists, for the calendar views.
function dueOn(tasks, lists, key) {
    const visible = visibleListIds(lists, "")
    return (tasks || []).filter(function(task) {
        return !task.completed && String(task.dueDate || "") === key
                && visible.indexOf(String(task.listId)) >= 0
    }).sort(compareTasks)
}
