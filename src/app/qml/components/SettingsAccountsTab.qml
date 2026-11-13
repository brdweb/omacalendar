pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../StatusLabels.js" as StatusLabels

// Connected accounts and the forms that add Google, CalDAV and ICS sources.
ScrollView {
    id: tab
    required property var drawer

    // A visible label above a form field.
    component FieldLabel: Text {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        Layout.topMargin: Theme.spacingXS
        color: Theme.text
        font.pixelSize: Theme.smallFontSize
        font.weight: Font.DemiBold
    }

    // One form's outcome: a spinner while it is in flight, then the result,
    // which stays until dismissed or the form is sent again. Children are
    // extra buttons shown beside it.
    component SubmissionStatus: RowLayout {
        id: status
        required property string form
        required property var drawer
        objectName: form + "SubmissionStatus"
        visible: status.drawer.submissionForm === form
        Layout.fillWidth: true
        spacing: Theme.spacingSM
        BusyIndicator {
            visible: status.drawer.submissionPending
            running: visible
            implicitWidth: 18
            implicitHeight: 18
        }
        Text {
            objectName: status.form + "SubmissionText"
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: status.drawer.submissionStatusText()
            color: status.drawer.submissionToneColor()
            font.pixelSize: Theme.smallFontSize
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
        AppButton {
            objectName: status.form + "SubmissionDismiss"
            visible: !status.drawer.submissionPending
                     && status.drawer.submissionMessage.length > 0
            text: "×"
            toolTipText: qsTr("Dismiss")
            Accessible.name: qsTr("Dismiss message")
            compact: true
            quiet: true
            onClicked: status.drawer.dismissSubmission()
        }
    }

    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical.policy: ScrollBar.AsNeeded
    ColumnLayout {
        width: tab.drawer.width - 40
        x: 20
        spacing: Theme.spacingMD

        Item { Layout.preferredHeight: 5 }
        AppAccordionSection {
            title: qsTr("Connected accounts")
            detail: String(tab.drawer.accounts.length)
            expanded: true

            Repeater {
            model: tab.drawer.effectiveAccountsModel
            delegate: Rectangle {
                id: accountCard
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: accountRow.implicitHeight + 24
                radius: Theme.radiusLG
                color: Theme.background
                border.color: Theme.border

                RowLayout {
                    id: accountRow
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMD
                    spacing: Theme.spacingSM
                    StatusBadge {
                        dotOnly: true
                        text: StatusLabels.accountStatusLabel(
                                  accountCard.modelData.authStatus)
                        tone: accountCard.modelData.authStatus === "connected"
                              ? "success"
                              : accountCard.modelData.authStatus === "reauthorization_required"
                                ? "danger" : "warning"
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: accountCard.modelData.displayName
                                  || accountCard.modelData.principal
                                  || qsTr("Calendar account")
                            color: Theme.text
                            font.pixelSize: Theme.fontSize
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            objectName: "accountStatusText-" + String(accountCard.modelData.id)
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: qsTr("%1 · %2").arg(
                                      StatusLabels.providerLabel(
                                          accountCard.modelData.provider)).arg(
                                      StatusLabels.accountStatusLabel(
                                          accountCard.modelData.authStatus))
                            color: Theme.mutedText
                            font.pixelSize: Theme.microFontSize
                            elide: Text.ElideRight
                        }
                    }
                    AppButton {
                        visible: accountCard.modelData.provider !== "local"
                        text: accountCard.modelData.provider === "ics"
                              ? qsTr("Refresh") : qsTr("Sync")
                        compact: true
                        quiet: true
                        enabled: tab.drawer.connected && !tab.drawer.busy
                        onClicked: tab.drawer.syncAccountRequested(
                                       String(accountCard.modelData.id))
                    }
                    AppButton {
                        visible: accountCard.modelData.provider === "google"
                                 && (accountCard.modelData.authStatus
                                     === "reauthorization_required"
                                     || accountCard.modelData.authStatus
                                        === "disconnected")
                        text: qsTr("Reauthorize")
                        compact: true
                        primary: true
                        onClicked: tab.drawer.reauthorizeAccountRequested(
                                       String(accountCard.modelData.id))
                    }
                    AppButton {
                        visible: accountCard.modelData.provider === "caldav"
                                 || accountCard.modelData.provider === "ics"
                        text: accountCard.modelData.authStatus
                              === "reauthorization_required"
                              || accountCard.modelData.authStatus
                                 === "disconnected"
                              ? qsTr("Reconnect") : qsTr("Credentials")
                        compact: true
                        primary: accountCard.modelData.authStatus
                                 === "reauthorization_required"
                                 || accountCard.modelData.authStatus
                                    === "disconnected"
                        quiet: !primary
                        onClicked: tab.drawer.credentialDialogRef.openFor(
                                       accountCard.modelData)
                    }
                    AppButton {
                        visible: accountCard.modelData.provider !== "local"
                        text: qsTr("Remove")
                        quiet: true
                        compact: true
                        destructive: true
                        onClicked: tab.drawer.removeConfirmRef.openFor(accountCard.modelData)
                    }
                }
            }
            }

            EmptyState {
                visible: tab.drawer.accounts.length === 0
                Layout.fillWidth: true
                Layout.preferredHeight: 145
                iconText: "◌"
                title: qsTr("No connected accounts")
                description: qsTr("Local calendars still work without a network account.")
            }
        }

        AppAccordionSection {
            title: qsTr("Google Calendar")
            expanded: true

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: googleContent.implicitHeight + 28
                radius: Theme.radiusLG
                color: Theme.background
                border.color: Theme.border
                ColumnLayout {
                id: googleContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.spacingMD
                spacing: Theme.spacingSM
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: qsTr("Authorization opens in your browser and returns through a secure local callback. OmaCalendar asks for access to your calendars and events, your tasks, and free/busy information.")
                    color: Theme.mutedText
                    font.pixelSize: Theme.smallFontSize
                    wrapMode: Text.Wrap
                }
                AppTextField {
                    id: googleName
                    Layout.fillWidth: true
                    placeholderText: qsTr("Account label (optional)")
                }
                AppTextField {
                    id: googleClientId
                    visible: !tab.drawer.bundledGoogleOAuthAvailable
                             && !tab.drawer.googleOAuthConfigured
                    Layout.fillWidth: true
                    placeholderText: qsTr("Desktop OAuth client ID")
                    accessibleName: qsTr("Google Desktop OAuth client ID")
                    inputMethodHints: Qt.ImhNoAutoUppercase
                }
                AppButton {
                    Layout.fillWidth: true
                    text: qsTr("Continue with Google in browser")
                    primary: true
                    enabled: tab.drawer.connected && !tab.drawer.busy
                             && (tab.drawer.bundledGoogleOAuthAvailable
                                 || tab.drawer.googleOAuthConfigured
                                 || googleClientId.text.trim().length > 0)
                    onClicked: {
                        tab.drawer.beginSubmission("google")
                        if (tab.drawer.bundledGoogleOAuthAvailable)
                            tab.drawer.connectGoogleRequested(
                                        googleName.text.trim())
                        else if (tab.drawer.googleOAuthConfigured)
                            tab.drawer.connectGoogleRequested(
                                        googleName.text.trim())
                        else
                            tab.drawer.connectGoogleClientRequested(
                                        googleClientId.text.trim(),
                                        googleName.text.trim())
                    }
                }
                SubmissionStatus {
                    form: "google"
                    drawer: tab.drawer
                    AppButton {
                        objectName: "googleSignInCancel"
                        visible: tab.drawer.submissionPending
                                 && tab.drawer.googleSignInState === "waiting"
                        text: qsTr("Cancel")
                        compact: true
                        quiet: true
                        onClicked: tab.drawer.cancelGoogleSignInRequested()
                    }
                }
                }
            }
        }

        AppAccordionSection {
            title: qsTr("CalDAV")
            expanded: false

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: caldavContent.implicitHeight + 28
                radius: Theme.radiusLG
                color: Theme.background
                border.color: Theme.border
                ColumnLayout {
                id: caldavContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.spacingMD
                spacing: Theme.spacingSM
                FieldLabel { text: qsTr("Server address") }
                AppTextField {
                    id: caldavEndpoint
                    objectName: "caldavEndpointField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("https://caldav.example.com/")
                    label: qsTr("CalDAV server URL")
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                }
                AppButton {
                    objectName: "caldavHelpToggle"
                    text: caldavHelp.shown ? qsTr("Hide server addresses")
                                             : qsTr("Where do I find the server address?")
                    compact: true
                    quiet: true
                    onClicked: caldavHelp.shown = !caldavHelp.shown
                }
                ColumnLayout {
                    id: caldavHelp
                    objectName: "caldavHelp"
                    property bool shown: false
                    visible: shown
                    Layout.fillWidth: true
                    spacing: Theme.spacingXS
                    Repeater {
                        model: [
                            {"name": qsTr("Fastmail"),
                             "text": qsTr("https://caldav.fastmail.com/ with your Fastmail address and an app password from Settings → Privacy & Security.")},
                            {"name": qsTr("iCloud"),
                             "text": qsTr("https://caldav.icloud.com/ with your Apple Account email and an app-specific password from account.apple.com. Your usual Apple Account password does not work here.")},
                            {"name": qsTr("Nextcloud"),
                             "text": qsTr("https://cloud.example.com/remote.php/dav/ with your Nextcloud username and an app password from Personal settings → Security.")},
                            {"name": qsTr("Radicale"),
                             "text": qsTr("https://radicale.example.com/ (or http://localhost:5232/ on this computer) with the username and password set up on the server.")}
                        ]
                        delegate: Text {
                            required property var modelData
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: qsTr("%1: %2").arg(modelData.name).arg(modelData.text)
                            color: Theme.mutedText
                            font.pixelSize: Theme.smallFontSize
                            wrapMode: Text.Wrap
                        }
                    }
                }
                FieldLabel { text: qsTr("Username") }
                AppTextField {
                    id: caldavUser
                    objectName: "caldavUser"
                    Layout.fillWidth: true
                    placeholderText: qsTr("name@example.com")
                    label: qsTr("CalDAV username")
                    inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                }
                FieldLabel { text: qsTr("Password or app password") }
                RowLayout {
                    id: caldavPasswordRow
                    property bool passwordShown: false
                    Layout.fillWidth: true
                    spacing: Theme.spacingSM
                    AppTextField {
                        id: caldavPassword
                        objectName: "caldavPassword"
                        Layout.fillWidth: true
                        label: qsTr("CalDAV password or app password")
                        echoMode: caldavPasswordRow.passwordShown ? TextInput.Normal
                                                                  : TextInput.Password
                    }
                    AppButton {
                        id: caldavShowPassword
                        objectName: "caldavShowPassword"
                        text: caldavPasswordRow.passwordShown ? qsTr("Hide") : qsTr("Show")
                        Accessible.name: caldavPasswordRow.passwordShown
                                         ? qsTr("Hide password") : qsTr("Show password")
                        compact: true
                        quiet: true
                        onClicked: caldavPasswordRow.passwordShown =
                                   !caldavPasswordRow.passwordShown
                    }
                }
                FieldLabel { text: qsTr("Account label (optional)") }
                AppTextField {
                    id: caldavName
                    objectName: "caldavName"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Shown in the sidebar")
                    label: qsTr("CalDAV account label")
                }
                AppButton {
                    Layout.fillWidth: true
                    text: qsTr("Connect CalDAV")
                    primary: true
                    enabled: tab.drawer.connected && !tab.drawer.busy
                             && caldavEndpoint.text.trim().length > 0
                             && caldavUser.text.trim().length > 0
                             && caldavPassword.text.length > 0
                    onClicked: {
                        tab.drawer.beginSubmission("caldav")
                        tab.drawer.addCalDavRequested(caldavEndpoint.text.trim(),
                                               caldavUser.text.trim(),
                                               caldavPassword.text,
                                               caldavName.text.trim())
                        caldavPassword.text = ""
                    }
                }
                SubmissionStatus {
                    form: "caldav"
                    drawer: tab.drawer
                }
                }
            }
        }

        AppAccordionSection {
            title: qsTr("ICS subscription")
            expanded: false

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: icsContent.implicitHeight + 28
                radius: Theme.radiusLG
                color: Theme.background
                border.color: Theme.border
                ColumnLayout {
                id: icsContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.spacingMD
                spacing: Theme.spacingSM
                AppTextField {
                    id: icsName
                    Layout.fillWidth: true
                    placeholderText: qsTr("Subscription name (optional)")
                }
                AppTextField {
                    id: icsUrl
                    objectName: "icsUrlField"
                    accessibleName: qsTr("Calendar feed URL")
                    Layout.fillWidth: true
                    placeholderText: qsTr("https://example.com/calendar.ics")
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                }
                RowLayout {
                    Layout.fillWidth: true
                    AppTextField {
                        id: icsUser
                        Layout.fillWidth: true
                        placeholderText: qsTr("Username (optional)")
                    }
                    AppTextField {
                        id: icsPassword
                        Layout.fillWidth: true
                        placeholderText: qsTr("Password (optional)")
                        echoMode: TextInput.Password
                    }
                }
                AppButton {
                    Layout.fillWidth: true
                    text: qsTr("Add read-only subscription")
                    enabled: tab.drawer.connected && icsUrl.text.trim().length > 0
                    onClicked: {
                        tab.drawer.beginSubmission("ics")
                        tab.drawer.addIcsSubscriptionRequested(icsUrl.text.trim(),
                                                         icsUser.text.trim(),
                                                         icsPassword.text,
                                                         icsName.text.trim())
                        icsPassword.text = ""
                    }
                }
                SubmissionStatus {
                    form: "ics"
                    drawer: tab.drawer
                }
                }
            }
        }
        Item { Layout.preferredHeight: 12 }
    }
}
