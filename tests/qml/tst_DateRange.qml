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
}
