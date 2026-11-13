import QtQuick
import QtTest
import "../../src/app/qml/DateRange.js" as DateRange

TestCase {
    name: "DateRange"

    function isoDate(value) {
        return Qt.formatDate(value, "yyyy-MM-dd")
    }

    function test_iso_week_numbers() {
        compare(DateRange.isoWeekNumber(new Date(2026, 0, 1)), 1, "Thursday starts week 1")
        compare(DateRange.isoWeekNumber(new Date(2027, 0, 1)), 53, "2026 has 53 weeks")
        compare(DateRange.isoWeekNumber(new Date(2024, 11, 30)), 1, "late December can be week 1")
        compare(DateRange.isoWeekNumber(new Date(2026, 8, 28)), 40)
        compare(DateRange.isoWeekNumber(new Date(2026, 9, 4)), 40, "Sunday ends the ISO week")
    }

    function test_row_week_number_follows_the_rows_thursday() {
        compare(DateRange.rowWeekNumber(new Date(2026, 8, 28)), 40, "Monday-first row")
        compare(DateRange.rowWeekNumber(new Date(2026, 8, 27)), 40, "Sunday-first row")
        compare(DateRange.rowWeekNumber(new Date(2026, 11, 27)), 53)
    }

    function test_week_range_includes_entire_sidebar_month() {
        const range = DateRange.includeMonthGrid(
                        new Date(2026, 8, 19), new Date(2026, 8, 29),
                        new Date(2026, 8, 1), 7)
        compare(isoDate(range.start), "2026-08-30")
        compare(isoDate(range.end), "2026-10-10")
    }

    function test_sidebar_navigation_preserves_main_week_range() {
        const range = DateRange.includeMonthGrid(
                        new Date(2026, 8, 19), new Date(2026, 8, 29),
                        new Date(2026, 9, 1), 7)
        compare(isoDate(range.start), "2026-09-19")
        compare(isoDate(range.end), "2026-11-07")
    }

    function test_locale_first_day_controls_month_grid_boundary() {
        const range = DateRange.includeMonthGrid(
                        new Date(2026, 8, 20), new Date(2026, 8, 26),
                        new Date(2026, 8, 1), 1)
        compare(isoDate(range.start), "2026-08-31")
        compare(isoDate(range.end), "2026-10-11")
    }

    function test_year_range_remains_unchanged_when_it_contains_grid() {
        const range = DateRange.includeMonthGrid(
                        new Date(2026, 0, 1), new Date(2026, 11, 31),
                        new Date(2026, 8, 1), 7)
        compare(isoDate(range.start), "2026-01-01")
        compare(isoDate(range.end), "2026-12-31")
    }

    function isoDates(values) {
        return values.map(isoDate)
    }

    function test_saturday_week_start() {
        // Wednesday September 30 2026 sits in the week from Saturday the 26th.
        compare(isoDate(DateRange.startOfWeek(new Date(2026, 8, 30), 6)), "2026-09-26")
        compare(isoDate(DateRange.startOfWeek(new Date(2026, 8, 26), 6)), "2026-09-26",
                "a Saturday starts its own week")
        compare(isoDate(DateRange.startOfWeek(new Date(2026, 8, 25), 6)), "2026-09-19")
        // September 2026 starts on a Tuesday, so the grid opens on August 29.
        const range = DateRange.includeMonthGrid(
                        new Date(2026, 8, 20), new Date(2026, 8, 26),
                        new Date(2026, 8, 1), 6)
        compare(isoDate(range.start), "2026-08-29")
        compare(isoDate(range.end), "2026-10-09")
        // A Saturday-first row is labelled with the ISO week of its weekdays.
        compare(DateRange.rowWeekNumber(new Date(2026, 8, 26)), 40)
    }

    function test_week_view_days() {
        const wednesday = new Date(2026, 8, 30)
        compare(isoDates(DateRange.visibleDays(wednesday, 1, 7, false)),
                ["2026-09-28", "2026-09-29", "2026-09-30", "2026-10-01",
                 "2026-10-02", "2026-10-03", "2026-10-04"])
        compare(isoDates(DateRange.visibleDays(wednesday, 1, 7, true)),
                ["2026-09-28", "2026-09-29", "2026-09-30", "2026-10-01", "2026-10-02"])
        compare(isoDates(DateRange.visibleDays(wednesday, 6, 7, true)),
                ["2026-09-28", "2026-09-29", "2026-09-30", "2026-10-01", "2026-10-02"],
                "a Saturday-first work week is still Monday to Friday")
        compare(isoDates(DateRange.visibleDays(wednesday, 7, 7, false))[0], "2026-09-27")
        compare(isoDates(DateRange.visibleDays(wednesday, 1, 3, false)),
                ["2026-09-30", "2026-10-01", "2026-10-02"], "a run starts on the anchor")
        compare(isoDates(DateRange.visibleDays(wednesday, 1, 4, true)),
                ["2026-09-30", "2026-10-01", "2026-10-02", "2026-10-05"],
                "a run skips the weekend without getting shorter")
        compare(isoDates(DateRange.visibleDays(new Date(2026, 9, 3), 1, 2, true)),
                ["2026-10-05", "2026-10-06"], "a run from Saturday starts on Monday")
        compare(DateRange.visibleDays(wednesday, 1, 0, false).length, 7,
                "an unset or invalid length is a week")
        compare(DateRange.visibleDays(wednesday, 1, "9", false).length, 7)
        compare(DateRange.visibleDays(wednesday, 1, "5", false).length, 5)
    }

    function test_week_view_navigation() {
        const wednesday = new Date(2026, 8, 30)
        compare(isoDate(DateRange.shiftAnchor(wednesday, 1, 7, true, 1)), "2026-10-07")
        compare(isoDate(DateRange.shiftAnchor(wednesday, 1, 7, false, -1)), "2026-09-23")
        compare(isoDate(DateRange.shiftAnchor(wednesday, 1, 3, false, 1)), "2026-10-03")
        compare(isoDate(DateRange.shiftAnchor(wednesday, 1, 3, false, -1)), "2026-09-27")
        // Wednesday, Thursday, Friday; then Monday to Wednesday.
        compare(isoDate(DateRange.shiftAnchor(wednesday, 1, 3, true, 1)), "2026-10-03")
        compare(isoDates(DateRange.visibleDays(new Date(2026, 9, 3), 1, 3, true)),
                ["2026-10-05", "2026-10-06", "2026-10-07"])
        compare(isoDate(DateRange.shiftAnchor(new Date(2026, 9, 5), 1, 3, true, -1)),
                "2026-09-30", "back three weekdays from Monday")
    }

    function test_month_columns_without_weekends() {
        compare(DateRange.rowColumnOffsets(new Date(2026, 7, 31), false),
                [0, 1, 2, 3, 4, 5, 6])
        compare(DateRange.rowColumnOffsets(new Date(2026, 7, 31), true), [0, 1, 2, 3, 4],
                "Monday-first rows drop the last two columns")
        compare(DateRange.rowColumnOffsets(new Date(2026, 7, 30), true), [1, 2, 3, 4, 5],
                "Sunday-first rows drop the first and last")
        compare(DateRange.rowColumnOffsets(new Date(2026, 7, 29), true), [2, 3, 4, 5, 6],
                "Saturday-first rows drop the first two")
    }
}
