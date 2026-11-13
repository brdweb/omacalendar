import QtQuick

// The window's application shortcuts. The window supplies state through the
// properties and acts on the signals, so the key bindings can be tested
// without loading Main.qml.
Item {
    id: shortcuts

    // Single-key navigation (T, [, ], arrows, Q, Return, Delete) and event
    // copy and paste (Ctrl+C, Ctrl+X, Ctrl+V) are allowed:
    // no text field, editor or drawer has focus.
    property bool navigationEnabled: false
    // The selected event is a timed event that can be moved or resized.
    property bool nudgeEnabled: false
    property bool hasSelectedEvent: false
    // The selected event can be deleted, so cutting it is allowed.
    property bool canCutSelection: false
    // An event has been copied and can be pasted.
    property bool hasClipboardEvent: false
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
    signal copyRequested()
    signal cutRequested()
    signal pasteRequested()
    signal undoRequested()
    signal redoRequested()
    signal settingsRequested()
    signal syncRequested()
    signal importRequested()
    signal quitRequested()
    signal helpRequested()

    // Every shortcut, for the cheat sheet. Keep it in step with the
    // Shortcut items below; test_shortcut_cheat_sheet_lists_all checks it.
    readonly property var catalog: [
        {"section": qsTr("General"), "keys": "Ctrl+N", "action": qsTr("New event")},
        {"section": qsTr("General"), "keys": "Q, Ctrl+Shift+N", "action": qsTr("Quick add")},
        {"section": qsTr("General"), "keys": "Ctrl+F, /", "action": qsTr("Search")},
        {"section": qsTr("General"), "keys": "Ctrl+Shift+T", "action": qsTr("Show or hide tasks")},
        {"section": qsTr("General"), "keys": "Ctrl+P", "action": qsTr("Print or save as PDF")},
        {"section": qsTr("General"), "keys": "Ctrl+O", "action": qsTr("Import an iCalendar file")},
        {"section": qsTr("General"), "keys": "Ctrl+R, F5", "action": qsTr("Sync now")},
        {"section": qsTr("General"), "keys": "Ctrl+,", "action": qsTr("Settings")},
        {"section": qsTr("General"), "keys": "Ctrl+Z", "action": qsTr("Undo")},
        {"section": qsTr("General"), "keys": "Ctrl+Shift+Z, Ctrl+Y", "action": qsTr("Redo")},
        {"section": qsTr("General"), "keys": "?", "action": qsTr("Keyboard shortcuts")},
        {"section": qsTr("General"), "keys": "Ctrl+Q", "action": qsTr("Quit")},
        {"section": qsTr("Views"), "keys": "Alt+1 … Alt+5", "action": qsTr("Agenda, day, week, month, year")},
        {"section": qsTr("Views"), "keys": "T", "action": qsTr("Go to today")},
        {"section": qsTr("Views"), "keys": "[, ]", "action": qsTr("Previous or next period")},
        {"section": qsTr("Views"), "keys": "Left, Right", "action": qsTr("Previous or next day")},
        {"section": qsTr("Events"), "keys": "Up, Down", "action": qsTr("Select previous or next event")},
        {"section": qsTr("Events"), "keys": "Enter", "action": qsTr("Open the selected event")},
        {"section": qsTr("Events"), "keys": "Delete", "action": qsTr("Delete the selected event")},
        {"section": qsTr("Events"), "keys": "Ctrl+C", "action": qsTr("Copy the selected event")},
        {"section": qsTr("Events"), "keys": "Ctrl+X", "action": qsTr("Cut the selected event")},
        {"section": qsTr("Events"), "keys": "Ctrl+V", "action": qsTr("Paste the copied event on the selected day")},
        {"section": qsTr("Events"), "keys": "Alt+Up, Alt+Down", "action": qsTr("Move the selected event 15 minutes")},
        {"section": qsTr("Events"), "keys": "Alt+Shift+Up, Alt+Shift+Down", "action": qsTr("Change the selected event's end by 15 minutes")},
        {"section": qsTr("Events"), "keys": "Alt+Left, Alt+Right", "action": qsTr("Move the selected event a day")},
        {"section": qsTr("Editors"), "keys": "Ctrl+Enter, Ctrl+S", "action": qsTr("Save")},
        {"section": qsTr("Editors"), "keys": "Escape", "action": qsTr("Close, asking before discarding changes")}
    ]

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
    // Event copy and paste, only while navigating: a text field keeps its
    // own Ctrl+C, Ctrl+X and Ctrl+V.
    Shortcut {
        sequence: "Ctrl+C"
        enabled: shortcuts.navigationEnabled && shortcuts.hasSelectedEvent
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.copyRequested()
    }
    Shortcut {
        sequence: "Ctrl+X"
        enabled: shortcuts.navigationEnabled && shortcuts.canCutSelection
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.cutRequested()
    }
    Shortcut {
        sequence: "Ctrl+V"
        enabled: shortcuts.navigationEnabled && shortcuts.hasClipboardEvent
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.pasteRequested()
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
    Shortcut {
        sequence: "Ctrl+,"
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.settingsRequested()
    }
    Shortcut {
        sequences: ["Ctrl+R", "F5"]
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.syncRequested()
    }
    Shortcut {
        sequence: "Ctrl+O"
        enabled: !shortcuts.editorOpen
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.importRequested()
    }
    Shortcut {
        sequence: "Ctrl+Q"
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.quitRequested()
    }
    Shortcut {
        sequence: "?"
        enabled: shortcuts.navigationEnabled
        context: Qt.ApplicationShortcut
        onActivated: shortcuts.helpRequested()
    }
}
