import QtQuick
import QtTest
import "../../src/app/qml/CalendarVisibility.js" as CalendarVisibility

TestCase {
    name: "CalendarVisibility"

    readonly property var calendars: [
        {"id": "work", "enabled": true},
        {"id": "family", "enabled": true},
        {"id": "hidden", "enabled": false}
    ]
    readonly property var sets: [
        {"id": "all-calendars", "calendarIds": ["work", "family", "hidden"]},
        {"id": "focus", "calendarIds": ["work"]},
        {"id": "empty", "calendarIds": []}
    ]

    function ids(values) {
        return values.map(function(value) { return String(value.id) })
    }

    function test_persistently_hidden_calendar_is_removed_from_sidebar() {
        compare(ids(CalendarVisibility.calendarsForSidebar(
                        calendars, sets, "all-calendars", {})),
                ["work", "family"])
    }

    function test_active_set_only_shows_its_visible_members() {
        compare(ids(CalendarVisibility.calendarsForSidebar(
                        calendars, sets, "focus", {})), ["work"])
    }

    function test_empty_set_shows_no_calendars_or_events() {
        compare(CalendarVisibility.calendarsForSidebar(
                    calendars, sets, "empty", {}), [])
        compare(CalendarVisibility.filterEvents(
                    [{"calendarId": "work"}], calendars, sets, "empty", {}), [])
    }

    function test_optimistic_visibility_override_updates_sidebar_and_events() {
        const overrides = {"family": false}
        compare(ids(CalendarVisibility.calendarsForSidebar(
                        calendars, sets, "all-calendars", overrides)), ["work"])
        const events = [
            {"id": "work-event", "calendarId": "work"},
            {"id": "family-event", "calendarId": "family"},
            {"id": "hidden-event", "calendarId": "hidden"}
        ]
        compare(ids(CalendarVisibility.filterEvents(
                        events, calendars, sets, "all-calendars", overrides)),
                ["work-event"])
    }
}
