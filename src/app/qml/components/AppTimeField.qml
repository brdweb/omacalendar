pragma ComponentBehavior: Bound
import QtQuick
import OmaCalendar
import "../DateEntry.js" as DateEntry

// A time of day entry. The user types and reads the time in the
// timeFormat preference ("12h", "24h" or "system"), and the entry is
// lenient: 9, 9:30, 9am, 9:30 pm, 09:00 and 21:00 all work.
//
// `text` is always the canonical 24-hour HH:mm while the entry parses,
// whatever is on screen, so the editors and App.wallTimeToUtc never see
// locale text. Entry that does not parse is kept in `text` as typed, so
// validation still rejects it.
FocusScope {
    id: control

    property string text: ""
    property string timeFormat: "system"
    // The canonical time the placeholder shows, in the current format.
    property string placeholderTime: "09:00"
    property string accessibleName: ""
    readonly property bool acceptableInput: DateEntry.parseTime(text) !== ""
    readonly property string displayText: input.text
    readonly property string displayPattern:
        DateEntry.minutePattern(Theme.timePattern(timeFormat))
    property bool updatingFromInput: false

    signal accepted()
    signal editingFinished()

    implicitWidth: input.implicitWidth
    implicitHeight: input.implicitHeight
    activeFocusOnTab: true

    function parse(value) {
        const locale = Qt.locale()
        return DateEntry.parseTime(value, locale.amText, locale.pmText)
    }

    function format(canonical) {
        const value = DateEntry.timeValue(canonical)
        return value ? Qt.formatTime(value, displayPattern) : canonical
    }

    function refreshDisplay() {
        const canonical = parse(text)
        input.text = canonical ? format(canonical) : text
    }

    // A value set from outside is normalized, so `text` stays canonical.
    onTextChanged: {
        if (updatingFromInput)
            return
        const canonical = parse(text)
        if (canonical && canonical !== text)
            text = canonical
        else
            refreshDisplay()
    }
    onDisplayPatternChanged: refreshDisplay()
    Component.onCompleted: refreshDisplay()

    AppTextField {
        id: input
        objectName: control.objectName.length > 0 ? control.objectName + "Input" : ""
        anchors.fill: parent
        focus: true
        placeholderText: control.format(control.placeholderTime)
        accessibleName: control.accessibleName
        inputMethodHints: Qt.ImhTime
        onTextEdited: {
            const canonical = control.parse(text)
            control.updatingFromInput = true
            control.text = canonical ? canonical : text.trim()
            control.updatingFromInput = false
        }
        onEditingFinished: {
            control.refreshDisplay()
            control.editingFinished()
        }
        onAccepted: control.accepted()
    }
}
