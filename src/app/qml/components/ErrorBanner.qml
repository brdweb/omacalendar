pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import OmaCalendar

// The window's error strip: what went wrong in plain words, the one action
// that would fix it, the service's own text under Details, and a dismiss
// button.
Rectangle {
    id: root

    property string message: ""
    property string details: ""
    // "sign_in", "account_settings", "retry" or "".
    property string action: ""
    property bool connected: true
    property bool detailsShown: false

    // The action button's label; empty hides it. Reconnecting to the
    // calendar service comes first because nothing else works without it.
    readonly property string actionText: {
        if (!connected)
            return qsTr("Reconnect")
        if (action === "sign_in")
            return qsTr("Sign in")
        if (action === "account_settings")
            return qsTr("Open account settings")
        if (action === "retry")
            return qsTr("Retry")
        return ""
    }

    // "reconnect" or the action, when the button is pressed.
    signal actionRequested(string action)
    signal dismissRequested()
    signal activityRequested()

    visible: message.length > 0
    implicitHeight: column.implicitHeight + 2 * Theme.spacingSM
    color: Theme.alpha(Theme.danger, 0.1)
    border.color: Theme.alpha(Theme.danger, 0.3)
    Accessible.role: Accessible.AlertMessage
    Accessible.name: message

    onMessageChanged: detailsShown = false

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.leftMargin: Theme.spacingMD
        anchors.rightMargin: Theme.spacingSM
        anchors.topMargin: Theme.spacingSM
        anchors.bottomMargin: Theme.spacingSM
        spacing: Theme.spacingXS

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSM
            StatusBadge { dotOnly: true; text: qsTr("Error"); tone: "danger" }
            Text {
                objectName: "errorBannerMessage"
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.message
                color: Theme.text
                font.pixelSize: Theme.smallFontSize
                wrapMode: Text.Wrap
            }
            AppButton {
                objectName: "errorBannerAction"
                visible: root.actionText.length > 0
                text: root.actionText
                compact: true
                primary: root.connected && root.action === "sign_in"
                onClicked: root.actionRequested(root.connected ? root.action
                                                               : "reconnect")
            }
            AppButton {
                objectName: "errorBannerDetails"
                text: root.details.length > 0 && root.detailsShown
                      ? qsTr("Hide details") : qsTr("Details")
                compact: true
                quiet: true
                onClicked: {
                    if (root.details.length > 0)
                        root.detailsShown = !root.detailsShown
                    else
                        root.activityRequested()
                }
            }
            AppButton {
                objectName: "errorBannerDismiss"
                text: "×"
                toolTipText: qsTr("Dismiss")
                Accessible.name: qsTr("Dismiss error")
                compact: true
                quiet: true
                onClicked: root.dismissRequested()
            }
        }

        RowLayout {
            objectName: "errorBannerDetailsRow"
            visible: root.detailsShown && root.details.length > 0
            Layout.fillWidth: true
            spacing: Theme.spacingSM
            Text {
                objectName: "errorBannerDetailsText"
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.details
                color: Theme.mutedText
                font.pixelSize: Theme.microFontSize
                wrapMode: Text.Wrap
            }
            AppButton {
                text: qsTr("Activity")
                compact: true
                quiet: true
                onClicked: root.activityRequested()
            }
        }
    }
}
