pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import OmaCalendar

// The calendar area's first-run and no-service states. The built-in device
// calendar always exists, so "first run" means no Google, CalDAV or ICS
// account yet and the welcome not dismissed.
Rectangle {
    id: root

    property bool connected: false
    property bool busy: false
    // True once accounts and preferences have loaded, so the welcome does not
    // flash before them.
    property bool ready: false
    property var accounts: []
    property bool onboardingDismissed: false

    // "welcome", "disconnected" or "".
    readonly property string mode: stateFor(connected, busy, ready, accounts,
                                            onboardingDismissed)

    signal addCalendarRequested()
    signal dismissRequested()
    signal reconnectRequested()

    function hasRemoteAccount(values) {
        const list = values || []
        for (let index = 0; index < list.length; ++index) {
            const provider = String((list[index] || {}).provider || "")
            if (provider.length > 0 && provider !== "local")
                return true
        }
        return false
    }

    function stateFor(isConnected, isBusy, isReady, accountList, dismissed) {
        if (!isConnected)
            return isBusy ? "" : "disconnected"
        if (!isReady || dismissed || hasRemoteAccount(accountList))
            return ""
        return "welcome"
    }

    visible: mode.length > 0
    implicitWidth: 420
    implicitHeight: emptyState.implicitHeight + 2 * Theme.spacingLG
    radius: Theme.radiusLG
    color: Theme.surface
    border.color: Theme.border

    EmptyState {
        id: emptyState
        objectName: root.mode === "disconnected" ? "serviceStoppedState"
                                                 : "welcomeState"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: Theme.spacingLG
        iconText: root.mode === "disconnected" ? "!" : Theme.glyphCalendar
        title: root.mode === "disconnected"
               ? qsTr("The calendar service isn't running")
               : qsTr("Your calendar, locally first")
        description: root.mode === "disconnected"
                     ? qsTr("OmaCalendar keeps your calendars in a background service. Reconnect to start it again; your events are safe on this device.")
                     : qsTr("Events you add now stay on this device. Connect Google, CalDAV, or an ICS subscription to bring in other calendars.")
        actionText: root.mode === "disconnected" ? qsTr("Reconnect")
                                                 : qsTr("Add a calendar")
        secondaryActionText: root.mode === "welcome" ? qsTr("Not now") : ""
        onActionRequested: {
            if (root.mode === "disconnected")
                root.reconnectRequested()
            else
                root.addCalendarRequested()
        }
        onSecondaryActionRequested: root.dismissRequested()
    }
}
