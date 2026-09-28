pragma ComponentBehavior: Bound
// App is intentionally supplied as a context property.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

// Lets the user build the final version of an event whose local and remote
// edits conflicted, starting from either saved version.
Dialog {
    id: conflictMergeDialog
    property var conflictData: ({})
    property var sourceSnapshot: ({})
    property string validationError: ""

    anchors.centerIn: Overlay.overlay
    width: Math.min(680, Overlay.overlay ? Overlay.overlay.width - 48 : 680)
    height: Math.min(760, Overlay.overlay ? Overlay.overlay.height - 48 : 760)
    modal: true
    title: qsTr("Merge conflicting event")
    standardButtons: Dialog.Cancel
    closePolicy: Popup.CloseOnEscape

    function openFor(value) {
        conflictData = value || ({})
        const local = conflictData.localSnapshot || ({})
        sourceVersionBox.currentIndex = Object.keys(local).length > 0 ? 0 : 1
        loadSnapshot()
        open()
        mergeTitleField.forceActiveFocus()
    }

    function selectedSnapshot() {
        if (sourceVersionBox.currentIndex === 0)
            return conflictData.localSnapshot || ({})
        return conflictData.remoteSnapshot || ({})
    }

    function wallText(value, endValue) {
        if (value.allDay)
            return String(endValue ? value.endDate : value.startDate)
        const utcText = String(endValue ? value.endUtc : value.startUtc)
        if (value.timeKind === "floating")
            return utcText.slice(0, 23)
        return App.utcToWallTime(utcText, value.startTimeZone || "")
    }

    function systemTimeZone() {
        try {
            return Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC"
        } catch (error) {
            return "UTC"
        }
    }

    function loadSnapshot() {
        sourceSnapshot = Object.assign({}, selectedSnapshot())
        validationError = ""
        mergeTitleField.text = sourceSnapshot.summary || ""
        mergeLocationField.text = sourceSnapshot.location || ""
        mergeUrlField.text = sourceSnapshot.url || ""
        mergeNotesField.text = sourceSnapshot.description || ""
        mergeAttendeesField.text = attendeeText(sourceSnapshot.attendees || [])
        mergeAllDay.checked = sourceSnapshot.allDay === true
        mergeTimeKind.currentIndex = sourceSnapshot.timeKind === "floating" ? 1 : 0
        mergeTimeZoneField.text = sourceSnapshot.startTimeZone
                || systemTimeZone()
        const startWall = wallText(sourceSnapshot, false)
        const endWall = wallText(sourceSnapshot, true)
        mergeStartDate.text = String(startWall).slice(0, 10)
        mergeStartTime.text = String(startWall).slice(11, 16)
        if (sourceSnapshot.allDay) {
            const exclusiveEnd = new Date(String(endWall) + "T00:00:00")
            exclusiveEnd.setDate(exclusiveEnd.getDate() - 1)
            mergeEndDate.text = Qt.formatDate(exclusiveEnd, "yyyy-MM-dd")
        } else {
            mergeEndDate.text = String(endWall).slice(0, 10)
        }
        mergeEndTime.text = String(endWall).slice(11, 16)
        mergeAvailability.currentIndex = sourceSnapshot.transparency
                === "transparent" ? 1 : 0
        mergeVisibility.currentIndex = visibilityIndex(
                    sourceSnapshot.visibility || "default")
        mergeRecurrenceField.text = sourceSnapshot.recurrenceRule || ""
    }

    function attendeeText(values) {
        const result = []
        for (let index = 0; index < values.length; ++index) {
            const attendee = values[index]
            result.push(typeof attendee === "string"
                        ? attendee : String(attendee.email || ""))
        }
        return result.filter(function(value) { return value.length > 0 }).join(", ")
    }

    function parsedAttendees() {
        const values = mergeAttendeesField.text.split(/[\n,;]/)
        const result = []
        const existing = sourceSnapshot.attendees || []
        for (let index = 0; index < values.length; ++index) {
            const email = values[index].trim()
            if (email.length === 0)
                continue
            let preserved = null
            for (let candidateIndex = 0; candidateIndex < existing.length;
                 ++candidateIndex) {
                const candidate = existing[candidateIndex]
                if (String(candidate.email || "").toLowerCase()
                        === email.toLowerCase()) {
                    preserved = Object.assign({}, candidate)
                    break
                }
            }
            if (preserved) {
                preserved.email = email
                result.push(preserved)
            } else {
                result.push({"email": email})
            }
        }
        return result
    }

    function visibilityIndex(value) {
        const values = ["default", "public", "private", "confidential"]
        const index = values.indexOf(String(value))
        return index < 0 ? 0 : index
    }

    function floatingIso(dateText, timeText) {
        return dateText + "T" + timeText + ":00.000Z"
    }

    function submitMerge() {
        validationError = ""
        if (!mergeTitleField.text.trim()) {
            validationError = qsTr("Add an event title.")
            return
        }
        const start = new Date(mergeStartDate.text + "T"
                               + (mergeAllDay.checked ? "00:00" : mergeStartTime.text))
        const end = new Date(mergeEndDate.text + "T"
                             + (mergeAllDay.checked ? "00:00" : mergeEndTime.text))
        if (isNaN(start.getTime()) || isNaN(end.getTime()) || end < start
                || (!mergeAllDay.checked && end <= start)) {
            validationError = qsTr("Enter a valid end after the start.")
            return
        }
        if (!mergeAllDay.checked && mergeTimeKind.currentValue !== "floating"
                && !App.isValidTimeZone(mergeTimeZoneField.text.trim())) {
            validationError = qsTr("Enter a valid IANA time zone.")
            return
        }

        const merged = Object.assign({}, sourceSnapshot)
        merged.summary = mergeTitleField.text.trim()
        merged.location = mergeLocationField.text.trim()
        merged.url = mergeUrlField.text.trim()
        merged.description = mergeNotesField.text
        merged.attendees = parsedAttendees()
        merged.allDay = mergeAllDay.checked
        merged.timeKind = mergeAllDay.checked ? "all_day"
                                              : mergeTimeKind.currentValue
        merged.transparency = mergeAvailability.currentValue
        merged.visibility = mergeVisibility.currentValue
        merged.recurrenceRule = mergeRecurrenceField.text.trim()
        if (mergeAllDay.checked) {
            const exclusiveEnd = new Date(mergeEndDate.text + "T00:00:00")
            exclusiveEnd.setDate(exclusiveEnd.getDate() + 1)
            merged.startDate = mergeStartDate.text
            merged.endDate = Qt.formatDate(exclusiveEnd, "yyyy-MM-dd")
            merged.startUtc = ""
            merged.endUtc = ""
            merged.startTimeZone = ""
            merged.endTimeZone = ""
        } else {
            merged.startTimeZone = mergeTimeKind.currentValue === "floating"
                    ? "" : mergeTimeZoneField.text.trim()
            merged.endTimeZone = merged.startTimeZone
            merged.startUtc = mergeTimeKind.currentValue === "floating"
                    ? floatingIso(mergeStartDate.text, mergeStartTime.text)
                    : App.wallTimeToUtc(mergeStartDate.text,
                                        mergeStartTime.text,
                                        merged.startTimeZone)
            merged.endUtc = mergeTimeKind.currentValue === "floating"
                    ? floatingIso(mergeEndDate.text, mergeEndTime.text)
                    : App.wallTimeToUtc(mergeEndDate.text,
                                        mergeEndTime.text,
                                        merged.endTimeZone)
            if (!merged.startUtc || !merged.endUtc) {
                validationError = qsTr("That wall time does not exist in the selected time zone.")
                return
            }
            merged.startDate = ""
            merged.endDate = ""
        }
        App.resolveConflict(String(conflictData.id), "merge", merged)
        close()
    }

    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: conflictMergeDialog.availableWidth
            spacing: Theme.spacingMD

            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: qsTr("Start from either saved version, then edit the final event. Provider identity and unsupported fields are preserved.")
                color: Theme.mutedText
                wrapMode: Text.Wrap
                font.pixelSize: Theme.smallFontSize
            }
            AppComboBox {
                id: sourceVersionBox
                Layout.fillWidth: true
                model: [qsTr("Start with my version"), qsTr("Start with remote version")]
                Accessible.name: qsTr("Conflict merge starting version")
                onActivated: conflictMergeDialog.loadSnapshot()
            }
            AppTextField {
                id: mergeTitleField
                Layout.fillWidth: true
                placeholderText: qsTr("Title")
                accessibleName: qsTr("Merged event title")
            }
            RowLayout {
                Layout.fillWidth: true
                AppCheckBox {
                    id: mergeAllDay
                    text: qsTr("All day")
                    Accessible.name: text
                }
                AppComboBox {
                    id: mergeTimeKind
                    Layout.fillWidth: true
                    visible: !mergeAllDay.checked
                    model: [{"text": qsTr("Zoned time"), "value": "zoned"},
                            {"text": qsTr("Floating time"), "value": "floating"}]
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Merged event time type")
                }
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.spacingSM
                rowSpacing: Theme.spacingSM
                AppTextField {
                    id: mergeStartDate
                    Layout.fillWidth: true
                    placeholderText: qsTr("Start date · YYYY-MM-DD")
                    accessibleName: qsTr("Merged event start date")
                }
                AppTextField {
                    id: mergeStartTime
                    Layout.fillWidth: true
                    visible: !mergeAllDay.checked
                    placeholderText: qsTr("Start time · HH:MM")
                    accessibleName: qsTr("Merged event start time")
                }
                AppTextField {
                    id: mergeEndDate
                    Layout.fillWidth: true
                    placeholderText: qsTr("End date · YYYY-MM-DD")
                    accessibleName: qsTr("Merged event end date")
                }
                AppTextField {
                    id: mergeEndTime
                    Layout.fillWidth: true
                    visible: !mergeAllDay.checked
                    placeholderText: qsTr("End time · HH:MM")
                    accessibleName: qsTr("Merged event end time")
                }
            }
            AppTextField {
                id: mergeTimeZoneField
                Layout.fillWidth: true
                visible: !mergeAllDay.checked
                         && mergeTimeKind.currentValue !== "floating"
                placeholderText: qsTr("IANA time zone · Europe/London")
                accessibleName: qsTr("Merged event time zone")
            }
            AppTextField {
                id: mergeLocationField
                Layout.fillWidth: true
                placeholderText: qsTr("Location")
                accessibleName: qsTr("Merged event location")
            }
            AppTextField {
                id: mergeUrlField
                Layout.fillWidth: true
                placeholderText: qsTr("URL or meeting link")
                accessibleName: qsTr("Merged event URL")
            }
            AppTextField {
                id: mergeAttendeesField
                Layout.fillWidth: true
                placeholderText: qsTr("Guest emails, separated by commas")
                accessibleName: qsTr("Merged event guests")
            }
            RowLayout {
                Layout.fillWidth: true
                AppComboBox {
                    id: mergeAvailability
                    Layout.fillWidth: true
                    model: [{"text": qsTr("Busy"), "value": "opaque"},
                            {"text": qsTr("Free"), "value": "transparent"}]
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Merged event availability")
                }
                AppComboBox {
                    id: mergeVisibility
                    Layout.fillWidth: true
                    model: [{"text": qsTr("Default visibility"), "value": "default"},
                            {"text": qsTr("Public"), "value": "public"},
                            {"text": qsTr("Private"), "value": "private"},
                            {"text": qsTr("Confidential"), "value": "confidential"}]
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Merged event visibility")
                }
            }
            AppTextField {
                id: mergeRecurrenceField
                Layout.fillWidth: true
                placeholderText: qsTr("Recurrence rule, for example FREQ=WEEKLY")
                accessibleName: qsTr("Merged event recurrence rule")
            }
            TextArea {
                id: mergeNotesField
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                placeholderText: qsTr("Notes")
                color: Theme.text
                wrapMode: TextEdit.Wrap
                Accessible.name: qsTr("Merged event notes")
            }
            Text {
                textFormat: Text.PlainText
                visible: conflictMergeDialog.validationError.length > 0
                Layout.fillWidth: true
                text: conflictMergeDialog.validationError
                color: Theme.danger
                wrapMode: Text.Wrap
                font.pixelSize: Theme.smallFontSize
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Apply merged event")
                    primary: true
                    onClicked: conflictMergeDialog.submitMerge()
                }
            }
        }
    }
}
