import QtQuick

// The window's application shortcuts. The window supplies state through the
// properties and acts on the signals, so the key bindings can be tested
// without loading Main.qml.
Item {
    id: shortcuts

    // Single-key navigation (T, [, ], arrows, Q, Return, Delete) is allowed:
    // no text field, editor or drawer has focus.
    property bool navigationEnabled: false
    // The selected event is a timed event that can be moved or resized.
    property bool nudgeEnabled: false
    property bool hasSelectedEvent: false
    property bool tasksAvailable: false
    property bool editorOpen: false
    property bool settingsOpen: false

    signal toggleTasksRequested()
    signal printRequested()
    signal newEventRequested()
    signal quickAddRequested()
    signal searchRequested()
    signal viewRequested(string view)
    signal todayRequested()
    signal periodRequested(int direction)
    signal selectionDateMoveRequested(int days)
    signal adjacentEventRequested(int direction)
    signal openSelectionRequested()
    // Minutes to move the start, minutes to move the end, days to move both.
    signal nudgeRequested(int startMinutes, int endMinutes, int days)
    signal deleteSelectionRequested()
    signal undoRequested()
    signal redoRequested()

    Shortcut {
        sequence: "Ctrl+Shift+T"
        enabled: shortcuts.tasksAvailable
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.toggleTasksRequested()
    }
    Shortcut {
        sequence: "Ctrl+P"
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.printRequested()
    }
    Shortcut {
        sequence: "Ctrl+N"
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.newEventRequested()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+N", "Q"]
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.quickAddRequested()
    }
    Shortcut {
        sequence: "Ctrl+F"
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.searchRequested()
    }
    Shortcut {
        sequence: "/"
        enabled: !shortcuts.editorOpen && !shortcuts.settingsOpen
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.searchRequested()
    }
    Shortcut { sequence: "Alt+1"; context: Qt.ApplicationShortcut; onActivated: shortcuts.viewRequested("agenda") }
    Shortcut { sequence: "Alt+2"; context: Qt.ApplicationShortcut; onActivated: shortcuts.viewRequested("day") }
    Shortcut { sequence: "Alt+3"; context: Qt.ApplicationShortcut; onActivated: shortcuts.viewRequested("week") }
    Shortcut { sequence: "Alt+4"; context: Qt.ApplicationShortcut; onActivated: shortcuts.viewRequested("month") }
    Shortcut { sequence: "Alt+5"; context: Qt.ApplicationShortcut; onActivated: shortcuts.viewRequested("year") }
    Shortcut {
        sequence: "T"
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.todayRequested()
    }
    Shortcut {
        sequence: "["
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.periodRequested(-1)
    }
    Shortcut {
        sequence: "]"
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.periodRequested(1)
    }
    Shortcut {
        sequence: "Left"
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.selectionDateMoveRequested(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.selectionDateMoveRequested(1)
    }
    Shortcut {
        sequence: "Up"
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.adjacentEventRequested(-1)
    }
    Shortcut {
        sequence: "Down"
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.adjacentEventRequested(1)
    }
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.openSelectionRequested()
    }
    // Keyboard move and resize for the selected timed event, matching drag:
    // 15-minute steps, a day at a time sideways, and the same recurrence
    // scope prompt.
    Shortcut {
        sequence: "Alt+Up"
        enabled: shortcuts.navigationEnabled && shortcuts.nudgeEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.nudgeRequested(-15, 0, 0)
    }
    Shortcut {
        sequence: "Alt+Down"
        enabled: shortcuts.navigationEnabled && shortcuts.nudgeEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.nudgeRequested(15, 0, 0)
    }
    Shortcut {
        sequence: "Alt+Shift+Up"
        enabled: shortcuts.navigationEnabled && shortcuts.nudgeEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.nudgeRequested(0, -15, 0)
    }
    Shortcut {
        sequence: "Alt+Shift+Down"
        enabled: shortcuts.navigationEnabled && shortcuts.nudgeEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.nudgeRequested(0, 15, 0)
    }
    Shortcut {
        sequence: "Alt+Left"
        enabled: shortcuts.navigationEnabled && shortcuts.nudgeEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.nudgeRequested(0, 0, -1)
    }
    Shortcut {
        sequence: "Alt+Right"
        enabled: shortcuts.navigationEnabled && shortcuts.nudgeEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.nudgeRequested(0, 0, 1)
    }
    Shortcut {
        sequence: "Delete"
        enabled: shortcuts.navigationEnabled && shortcuts.hasSelectedEvent
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.deleteSelectionRequested()
    }
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: !shortcuts.editorOpen
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.undoRequested()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        enabled: !shortcuts.editorOpen
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.redoRequested()
    }
}
