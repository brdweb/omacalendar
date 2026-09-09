import QtQuick
import QtTest
import "../../src/app/qml/DateRange.js" as DateRange

TestCase {
    name: "DateRange"

    function isoDate(value) {
        return Qt.formatDate(value, "yyyy-MM-dd")
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
