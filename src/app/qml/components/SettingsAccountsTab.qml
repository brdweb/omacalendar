pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Connected accounts and the forms that add Google, CalDAV and ICS sources.
ScrollView {
    id: tab
    required property var drawer

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
                        text: accountCard.modelData.authStatus || qsTr("unknown")
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
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: String(accountCard.modelData.provider || qsTr("calendar")).toUpperCase()
                                  + " · "
                                  + String(accountCard.modelData.authStatus || qsTr("connected")).replace(/_/g, " ")
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
                    text: qsTr("Authorization opens in your browser and returns through a secure local callback. OmaCalendar requests calendar and event access only.")
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
                RowLayout {
                    objectName: "googleSubmissionStatus"
                    visible: tab.drawer.submissionForm === "google"
                    Layout.fillWidth: true
                    spacing: Theme.spacingSM
                    BusyIndicator {
                        visible: tab.drawer.submissionPending
                        running: visible
                        implicitWidth: 18
                        implicitHeight: 18
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: tab.drawer.submissionStatusText()
                        color: tab.drawer.submissionToneColor()
                        font.pixelSize: Theme.smallFontSize
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
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
                AppTextField {
                    id: caldavName
                    Layout.fillWidth: true
                    placeholderText: qsTr("Account label (optional)")
                }
                AppTextField {
                    id: caldavEndpoint
                    Layout.fillWidth: true
                    placeholderText: qsTr("https://caldav.example.com/")
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                }
                AppTextField {
                    id: caldavUser
                    Layout.fillWidth: true
                    placeholderText: qsTr("Username")
                    inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                }
                AppTextField {
                    id: caldavPassword
                    Layout.fillWidth: true
                    placeholderText: qsTr("Password or app password")
                    echoMode: TextInput.Password
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
                RowLayout {
                    objectName: "caldavSubmissionStatus"
                    visible: tab.drawer.submissionForm === "caldav"
                    Layout.fillWidth: true
                    spacing: Theme.spacingSM
                    BusyIndicator {
                        visible: tab.drawer.submissionPending
                        running: visible
                        implicitWidth: 18
                        implicitHeight: 18
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: tab.drawer.submissionStatusText()
                        color: tab.drawer.submissionToneColor()
                        font.pixelSize: Theme.smallFontSize
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
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
                RowLayout {
                    objectName: "icsSubmissionStatus"
                    visible: tab.drawer.submissionForm === "ics"
                    Layout.fillWidth: true
                    spacing: Theme.spacingSM
                    BusyIndicator {
                        visible: tab.drawer.submissionPending
                        running: visible
                        implicitWidth: 18
                        implicitHeight: 18
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: tab.drawer.submissionStatusText()
                        color: tab.drawer.submissionToneColor()
                        font.pixelSize: Theme.smallFontSize
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }
                }
            }
        }
        Item { Layout.preferredHeight: 12 }
    }
}
