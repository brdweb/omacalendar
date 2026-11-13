.pragma library

// Words for the daemon's internal state codes, so screens never show
// "reauthorization_required" or "GOOGLE".

function providerLabel(provider) {
    switch (String(provider || "")) {
    case "google":
        return qsTr("Google")
    case "caldav":
        return qsTr("CalDAV")
    case "ics":
        return qsTr("ICS subscription")
    case "local":
        return qsTr("On this device")
    default:
        return qsTr("Calendar account")
    }
}

// An account's authStatus.
function accountStatusLabel(status) {
    switch (String(status || "")) {
    case "":
    case "connected":
        return qsTr("Connected")
    case "reauthorization_required":
        return qsTr("Sign-in needed")
    case "authorizing":
        return qsTr("Waiting for sign-in")
    case "connecting":
        return qsTr("Connecting…")
    case "credential_storage_pending":
        return qsTr("Saving password…")
    case "disconnecting":
        return qsTr("Disconnecting…")
    case "disconnected":
        return qsTr("Disconnected")
    case "not_configured":
        return qsTr("Not set up")
    default:
        return qsTr("Unknown status")
    }
}

// A queued change's outbox state.
function operationStateLabel(state) {
    switch (String(state || "")) {
    case "":
    case "pending":
        return qsTr("Waiting to send")
    case "sending":
        return qsTr("Sending…")
    case "retry_wait":
        return qsTr("Will retry soon")
    case "blocked":
        return qsTr("Needs attention")
    case "done":
        return qsTr("Sent")
    default:
        return qsTr("Waiting to send")
    }
}
