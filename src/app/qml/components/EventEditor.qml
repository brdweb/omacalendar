import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar

Dialog {
    id: editor
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(520, Overlay.overlay ? Overlay.overlay.width - 48 : 520)
    padding: 24
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    property var eventData: ({})
    property bool editing: Boolean(eventData && eventData.id)
    readonly property var writableCalendars: App.calendars.filter(
                                                 function(calendar) {
                                                     return calendar.enabled && !calendar.readOnly
                                                 })
    signal saveRequested(var eventData)
    signal removeRequested(string eventId)

    function openNew(date) {
        eventData = ({})
        titleField.text = ""
        locationField.text = ""
        notesField.text = ""
        allDay.checked = false
        dateField.text = Qt.formatDate(date, "yyyy-MM-dd")
        startField.text = "09:00"
        endField.text = "10:00"
        calendarBox.currentIndex = writableCalendars.length > 0 ? 0 : -1
        open()
        titleField.forceActiveFocus()
    }

    function openExisting(value) {
        eventData = value || ({})
        titleField.text = eventData.summary || ""
        locationField.text = eventData.location || ""
        notesField.text = eventData.description || ""
        allDay.checked = eventData.allDay === true
        const start = eventData.allDay ? new Date(eventData.startDate + "T00:00:00")
                                       : new Date(eventData.startUtc)
        const end = eventData.allDay ? new Date(eventData.endDate + "T00:00:00")
                                     : new Date(eventData.endUtc)
        dateField.text = Qt.formatDate(start, "yyyy-MM-dd")
        startField.text = Qt.formatTime(start, "HH:mm")
        endField.text = Qt.formatTime(end, "HH:mm")
        calendarBox.currentIndex = -1
        for (let index = 0; index < writableCalendars.length; ++index) {
            if (writableCalendars[index].id === eventData.calendarId) {
                calendarBox.currentIndex = index
                break
            }
        }
        open()
        titleField.forceActiveFocus()
    }

    function dateTime(dateText, timeText) {
        return new Date(dateText + "T" + timeText + ":00")
    }

    function submit() {
        if (!titleField.text.trim() || calendarBox.currentIndex < 0) return
        let value = Object.assign({}, eventData || {})
        value.summary = titleField.text.trim()
        value.description = notesField.text
        value.location = locationField.text
        value.calendarId = writableCalendars[calendarBox.currentIndex].id
        value.allDay = allDay.checked
        value.startTimeZone = Intl.DateTimeFormat().resolvedOptions().timeZone
        value.endTimeZone = value.startTimeZone
        if (allDay.checked) {
            value.startDate = dateField.text
            value.endDate = Qt.formatDate(new Date(dateTime(dateField.text, "00:00").getTime() + 86400000), "yyyy-MM-dd")
            value.startUtc = ""
            value.endUtc = ""
        } else {
            value.startUtc = dateTime(dateField.text, startField.text).toISOString()
            value.endUtc = dateTime(dateField.text, endField.text).toISOString()
            value.startDate = ""
            value.endDate = ""
        }
        saveRequested(value)
        close()
    }

    background: Rectangle {
        radius: 18
        color: Theme.surface
        border.color: Theme.alpha(Theme.text, 0.14)
    }
    header: Item {
        implicitHeight: 64
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            text: editor.editing ? "Edit event" : "New event"
            color: Theme.text
            font.pixelSize: Theme.fontSize + 4
            font.weight: Font.Bold
        }
    }
    contentItem: ColumnLayout {
        spacing: 14
        TextField {
            id: titleField
            Layout.fillWidth: true
            placeholderText: "Event title"
            font.pixelSize: Theme.fontSize + 2
            color: Theme.text
            selectByMouse: true
        }
        ComboBox {
            id: calendarBox
            Layout.fillWidth: true
            model: editor.writableCalendars
            textRole: "name"
            valueRole: "id"
            enabled: !editor.editing
        }
        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: dateField
                Layout.fillWidth: true
                placeholderText: "YYYY-MM-DD"
                color: Theme.text
            }
            CheckBox {
                id: allDay
                text: "All day"
            }
        }
        RowLayout {
            visible: !allDay.checked
            TextField {
                id: startField
                Layout.fillWidth: true
                placeholderText: "09:00"
                color: Theme.text
            }
            Text { text: "to"; color: Theme.mutedText }
            TextField {
                id: endField
                Layout.fillWidth: true
                placeholderText: "10:00"
                color: Theme.text
            }
        }
        TextField {
            id: locationField
            Layout.fillWidth: true
            placeholderText: "Location"
            color: Theme.text
        }
        TextArea {
            id: notesField
            Layout.fillWidth: true
            Layout.preferredHeight: 92
            placeholderText: "Notes"
            color: Theme.text
            wrapMode: TextEdit.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            AppButton {
                visible: editor.editing && calendarBox.currentIndex >= 0
                text: "Delete"
                onClicked: {
                    editor.removeRequested(editor.eventData.id)
                    editor.close()
                }
            }
            Item { Layout.fillWidth: true }
            AppButton { text: "Cancel"; quiet: true; onClicked: editor.close() }
            AppButton {
                text: editor.editing ? "Save" : "Add event"
                primary: true
                enabled: titleField.text.trim().length > 0
                         && calendarBox.currentIndex >= 0
                onClicked: editor.submit()
            }
        }
    }
}
