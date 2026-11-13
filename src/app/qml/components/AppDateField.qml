pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OmaCalendar
import "../DateEntry.js" as DateEntry

// A date entry: type yyyy-MM-dd (yyyy-M-d and "/" or "." separators work
// too) or pick from a month grid that starts on firstDayOfWeek.
//
// `text` is always the canonical yyyy-MM-dd while the entry parses, so the
// editors keep reading ISO dates. Entry that does not parse is kept in
// `text` as typed, so validation still rejects it.
FocusScope {
    id: control

    property string text: ""
    // Qt's numbering: Monday is 1, Sunday is 7.
    property int firstDayOfWeek: Qt.locale().firstDayOfWeek
    property string placeholderText: qsTr("YYYY-MM-DD")
    property string accessibleName: ""
    readonly property bool acceptableInput: DateEntry.parseDate(text) !== ""
    readonly property string displayText: input.text
    readonly property alias pickerOpen: picker.opened
    // The month the picker shows and the day the keyboard is on.
    property date pickerMonth: new Date()
    property date highlightedDate: new Date()
    readonly property date today: new Date()
    property bool updatingFromInput: false

    signal accepted()
    signal editingFinished()
    // A day was chosen in the picker.
    signal picked()

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight
    activeFocusOnTab: true

    function refreshDisplay() {
        const canonical = DateEntry.parseDate(text)
        input.text = canonical ? canonical : text
    }

    function openPicker() {
        if (!control.enabled)
            return
        const current = DateEntry.dateFromIso(DateEntry.parseDate(text))
        const base = current ? current : new Date()
        highlightedDate = base
        pickerMonth = new Date(base.getFullYear(), base.getMonth(), 1)
        picker.open()
    }

    function moveHighlight(value) {
        highlightedDate = value
        if (value.getFullYear() !== pickerMonth.getFullYear()
                || value.getMonth() !== pickerMonth.getMonth())
            pickerMonth = new Date(value.getFullYear(), value.getMonth(), 1)
    }

    function choose(value) {
        text = DateEntry.formatIsoDate(value)
        picker.close()
        input.forceActiveFocus()
        editingFinished()
        picked()
    }

    // A value set from outside is normalized, so `text` stays canonical.
    onTextChanged: {
        if (updatingFromInput)
            return
        const canonical = DateEntry.parseDate(text)
        if (canonical && canonical !== text)
            text = canonical
        else
            refreshDisplay()
    }
    onVisibleChanged: {
        if (!visible)
            picker.close()
    }
    Component.onCompleted: refreshDisplay()

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: Theme.spacingXS

        AppTextField {
            id: input
            objectName: control.objectName.length > 0 ? control.objectName + "Input" : ""
            Layout.fillWidth: true
            Layout.minimumWidth: 96
            focus: true
            placeholderText: control.placeholderText
            accessibleName: control.accessibleName
            inputMethodHints: Qt.ImhDate
            onTextEdited: {
                const canonical = DateEntry.parseDate(text)
                control.updatingFromInput = true
                control.text = canonical ? canonical : text.trim()
                control.updatingFromInput = false
            }
            onEditingFinished: {
                control.refreshDisplay()
                control.editingFinished()
            }
            onAccepted: control.accepted()
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Down && (event.modifiers & Qt.AltModifier)) {
                    control.openPicker()
                    event.accepted = true
                }
            }
        }

        AppButton {
            id: pickerButton
            objectName: control.objectName.length > 0 ? control.objectName + "PickerButton" : ""
            compact: true
            quiet: true
            iconText: Theme.glyphCalendar
            toolTipText: qsTr("Choose a date (Alt+Down)")
            Accessible.name: control.accessibleName.length > 0
                             ? qsTr("Choose %1").arg(control.accessibleName)
                             : qsTr("Choose a date")
            Layout.preferredHeight: input.implicitHeight
            onClicked: control.openPicker()
        }
    }

    Popup {
        id: picker
        objectName: control.objectName.length > 0 ? control.objectName + "Picker" : ""
        y: control.height + Theme.spacingXS
        x: Math.max(0, control.width - width)
        padding: Theme.spacingSM
        focus: true
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        onClosed: {
            if (control.visible)
                input.forceActiveFocus()
        }

        background: Rectangle {
            radius: Theme.radiusMD
            color: Theme.surface
            border.width: 1
            border.color: Theme.border
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingXS
            focus: true
            // Escape closes the picker before an editor's Escape shortcut
            // can treat it as a request to dismiss the whole dialog.
            Keys.onShortcutOverride: event => {
                if (event.key === Qt.Key_Escape)
                    event.accepted = true
            }
            Keys.onEscapePressed: event => {
                picker.close()
                event.accepted = true
            }
            Keys.onPressed: event => {
                const current = control.highlightedDate
                let next = null
                if (event.key === Qt.Key_Left)
                    next = DateEntry.addDays(current, -1)
                else if (event.key === Qt.Key_Right)
                    next = DateEntry.addDays(current, 1)
                else if (event.key === Qt.Key_Up)
                    next = DateEntry.addDays(current, -7)
                else if (event.key === Qt.Key_Down)
                    next = DateEntry.addDays(current, 7)
                else if (event.key === Qt.Key_PageUp)
                    next = DateEntry.addMonths(current, -1)
                else if (event.key === Qt.Key_PageDown)
                    next = DateEntry.addMonths(current, 1)
                else if (event.key === Qt.Key_Home)
                    next = new Date()
                if (next) {
                    control.moveHighlight(next)
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Space) {
                    control.choose(current)
                    event.accepted = true
                }
            }

            RowLayout {
                Layout.fillWidth: true
                AppButton {
                    compact: true
                    quiet: true
                    focusPolicy: Qt.NoFocus
                    text: Theme.glyphPrevious
                    toolTipText: qsTr("Previous month (Page Up)")
                    Accessible.name: qsTr("Previous month")
                    onClicked: control.moveHighlight(
                                   DateEntry.addMonths(control.highlightedDate, -1))
                }
                Text {
                    objectName: control.objectName.length > 0
                                ? control.objectName + "PickerMonth" : ""
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    textFormat: Text.PlainText
                    text: Qt.locale().standaloneMonthName(control.pickerMonth.getMonth())
                          + " " + control.pickerMonth.getFullYear()
                    color: Theme.text
                    font.pixelSize: Theme.fontSize
                    font.weight: Font.DemiBold
                }
                AppButton {
                    compact: true
                    quiet: true
                    focusPolicy: Qt.NoFocus
                    text: Theme.glyphNext
                    toolTipText: qsTr("Next month (Page Down)")
                    Accessible.name: qsTr("Next month")
                    onClicked: control.moveHighlight(
                                   DateEntry.addMonths(control.highlightedDate, 1))
                }
            }

            Grid {
                id: dayGrid
                objectName: control.objectName.length > 0
                            ? control.objectName + "PickerGrid" : ""
                columns: 7
                spacing: 2
                readonly property date start:
                    DateEntry.gridStart(control.pickerMonth.getFullYear(),
                                        control.pickerMonth.getMonth(),
                                        control.firstDayOfWeek)
                readonly property var weekDays: DateEntry.weekDayOrder(control.firstDayOfWeek)

                Repeater {
                    model: dayGrid.weekDays
                    delegate: Text {
                        required property int modelData
                        width: 34
                        height: 22
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        textFormat: Text.PlainText
                        // Locale.dayName counts Sunday as 0.
                        text: Qt.locale().dayName(modelData % 7, Locale.ShortFormat)
                        color: Theme.mutedText
                        font.pixelSize: Theme.microFontSize
                    }
                }

                Repeater {
                    model: 42
                    delegate: Rectangle {
                        id: dayCell
                        required property int index
                        readonly property date day: DateEntry.addDays(dayGrid.start, index)
                        readonly property string iso: DateEntry.formatIsoDate(day)
                        readonly property bool inMonth:
                            day.getMonth() === control.pickerMonth.getMonth()
                        readonly property bool selected: iso === control.text
                        readonly property bool highlighted:
                            iso === DateEntry.formatIsoDate(control.highlightedDate)
                        readonly property bool isToday:
                            iso === DateEntry.formatIsoDate(control.today)
                        objectName: "day-" + iso
                        width: 34
                        height: 30
                        radius: Theme.radiusSM
                        color: selected ? Theme.accent
                                        : dayMouse.containsMouse ? Theme.accentSoft
                                                                 : "transparent"
                        border.width: highlighted ? 2 : (isToday ? 1 : 0)
                        border.color: highlighted ? Theme.focus : Theme.mutedText
                        Accessible.role: Accessible.Button
                        Accessible.name: Qt.formatDate(day, Locale.LongFormat)

                        Text {
                            anchors.centerIn: parent
                            textFormat: Text.PlainText
                            text: dayCell.day.getDate()
                            color: dayCell.selected ? Theme.accentText
                                                    : dayCell.inMonth ? Theme.text
                                                                      : Theme.mutedText
                            opacity: dayCell.inMonth || dayCell.selected ? 1 : 0.6
                            font.pixelSize: Theme.smallFontSize
                        }

                        MouseArea {
                            id: dayMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: control.choose(dayCell.day)
                        }
                    }
                }
            }
        }
    }
}
