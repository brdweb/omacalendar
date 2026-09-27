import QtQuick
import QtTest
import "../../src/app/qml/EventIndex.js" as EventIndex

TestCase {
    name: "EventIndex"

    function timed(id, start, end) {
        return {"id": id, "allDay": false,
                "displayStartLocal": start, "displayEndLocal": end}
    }

    function allDay(id, startDate, endDate) {
        return {"id": id, "allDay": true, "startDate": startDate, "endDate": endDate}
    }

    function ids(values) {
        return values.map(function(value) { return value.id })
    }

    function test_multi_day_events_cover_every_day_with_an_exclusive_end() {
        const index = EventIndex.build([
            allDay("trip", "2026-08-17", "2026-08-20"),
            timed("overnight", "2026-08-17T22:00:00", "2026-08-18T02:00:00"),
            timed("until-midnight", "2026-08-19T20:00:00", "2026-08-20T00:00:00")
        ])
        compare(ids(EventIndex.eventsForDate(index, new Date(2026, 7, 16))), [])
        compare(ids(EventIndex.eventsForDate(index, new Date(2026, 7, 17))),
                ["trip", "overnight"])
        compare(ids(EventIndex.eventsForDate(index, new Date(2026, 7, 18))),
                ["trip", "overnight"])
        compare(ids(EventIndex.eventsForDate(index, new Date(2026, 7, 19))),
                ["trip", "until-midnight"])
        compare(ids(EventIndex.eventsForDate(index, new Date(2026, 7, 20))), [])
    }

    function test_days_list_all_day_events_first_then_by_start() {
        const index = EventIndex.build([
            timed("late", "2026-08-17T15:00:00", "2026-08-17T16:00:00"),
            allDay("holiday", "2026-08-17", "2026-08-18"),
            timed("early", "2026-08-17T09:00:00", "2026-08-17T10:00:00")
        ])
        compare(ids(EventIndex.eventsForDate(index, new Date(2026, 7, 17, 12))),
                ["holiday", "early", "late"])
        compare(EventIndex.countForDate(index, new Date(2026, 7, 17)), 3)
        const entries = EventIndex.entriesForDate(index, new Date(2026, 7, 17))
        compare(entries[1].startMs, new Date(2026, 7, 17, 9).getTime())
    }

    function test_zero_length_and_invalid_events() {
        const index = EventIndex.build([
            timed("instant", "2026-08-17T14:00:00", "2026-08-17T14:00:00"),
            timed("broken", "not a date", "2026-08-17T14:00:00"),
            {"id": "empty", "allDay": true}
        ])
        compare(ids(EventIndex.eventsForDate(index, new Date(2026, 7, 17))), ["instant"])
        compare(EventIndex.countForDate(index, new Date(2026, 7, 18)), 0)
    }

    function test_very_long_events_are_bounded() {
        const index = EventIndex.build([allDay("decade", "2020-01-01", "2030-01-01")])
        compare(EventIndex.countForDate(index, new Date(2020, 0, 1)), 1)
        compare(EventIndex.countForDate(index, new Date(2021, 0, 1)), 1)
        compare(EventIndex.countForDate(index, new Date(2025, 0, 1)), 0)
    }

    function test_empty_input() {
        compare(EventIndex.eventsForDate(EventIndex.build([]), new Date()), [])
        compare(EventIndex.eventsForDate(EventIndex.build(undefined), new Date()), [])
        compare(EventIndex.eventsForDate(null, new Date()), [])
    }
}
