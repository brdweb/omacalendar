import QtQuick
import QtTest
import "../../src/app/qml/DateEntry.js" as DateEntry

TestCase {
    name: "DateEntry"

    function test_time_parse_data() {
        return [
            {"tag": "bare hour", "input": "9", "expected": "09:00"},
            {"tag": "hour and minutes", "input": "9:30", "expected": "09:30"},
            {"tag": "dot separator", "input": "9.30", "expected": "09:30"},
            {"tag": "padded 24h", "input": "09:00", "expected": "09:00"},
            {"tag": "afternoon 24h", "input": "21:00", "expected": "21:00"},
            {"tag": "compact digits", "input": "930", "expected": "09:30"},
            {"tag": "compact 24h", "input": "2145", "expected": "21:45"},
            {"tag": "seconds ignored", "input": "21:00:00", "expected": "21:00"},
            {"tag": "surrounding space", "input": "  7:05 ", "expected": "07:05"},
            {"tag": "am", "input": "9am", "expected": "09:00"},
            {"tag": "pm", "input": "9pm", "expected": "21:00"},
            {"tag": "pm with minutes and space", "input": "9:30 pm", "expected": "21:30"},
            {"tag": "upper case", "input": "9:30 PM", "expected": "21:30"},
            {"tag": "dotted", "input": "9:30 p.m.", "expected": "21:30"},
            {"tag": "single letter", "input": "11a", "expected": "11:00"},
            {"tag": "midnight 24h", "input": "0:00", "expected": "00:00"},
            {"tag": "midnight 24h padded", "input": "00:00", "expected": "00:00"},
            {"tag": "midnight 12h", "input": "12am", "expected": "00:00"},
            {"tag": "after midnight 12h", "input": "12:30 am", "expected": "00:30"},
            {"tag": "noon 24h", "input": "12:00", "expected": "12:00"},
            {"tag": "noon 12h", "input": "12pm", "expected": "12:00"},
            {"tag": "after noon 12h", "input": "12:15 PM", "expected": "12:15"},
            {"tag": "last minute", "input": "23:59", "expected": "23:59"},
            {"tag": "empty", "input": "", "expected": ""},
            {"tag": "spaces only", "input": "   ", "expected": ""},
            {"tag": "letters", "input": "soon", "expected": ""},
            {"tag": "hour 24", "input": "24:00", "expected": ""},
            {"tag": "hour 25", "input": "25", "expected": ""},
            {"tag": "minute 60", "input": "9:60", "expected": ""},
            {"tag": "one minute digit", "input": "9:5", "expected": ""},
            {"tag": "13pm", "input": "13pm", "expected": ""},
            {"tag": "0am", "input": "0am", "expected": ""},
            {"tag": "unknown suffix", "input": "9 xm", "expected": ""},
            {"tag": "too many digits", "input": "12345", "expected": ""},
            {"tag": "negative", "input": "-1", "expected": ""}
        ]
    }

    function test_time_parse(data) {
        compare(DateEntry.parseTime(data.input), data.expected)
    }

    function test_time_parse_locale_meridiem() {
        compare(DateEntry.parseTime("9:30 nachm.", "vorm.", "nachm."), "21:30")
        compare(DateEntry.parseTime("9:30 vorm.", "vorm.", "nachm."), "09:30")
        compare(DateEntry.parseTime("8 下午", "上午", "下午"), "20:00")
        compare(DateEntry.parseTime("12 上午", "上午", "下午"), "00:00")
        compare(DateEntry.parseTime("9 abends", "vorm.", "nachm."), "",
                "words that are not the locale's markers are rejected")
        compare(DateEntry.parseTime("9pm", "vorm.", "nachm."), "21:00",
                "English markers keep working in any locale")
    }

    function test_date_parse_data() {
        return [
            {"tag": "iso", "input": "2026-09-05", "expected": "2026-09-05"},
            {"tag": "unpadded", "input": "2026-9-5", "expected": "2026-09-05"},
            {"tag": "slashes", "input": "2026/12/31", "expected": "2026-12-31"},
            {"tag": "dots", "input": "2026.1.2", "expected": "2026-01-02"},
            {"tag": "spaces", "input": " 2026-09-05 ", "expected": "2026-09-05"},
            {"tag": "leap day", "input": "2028-02-29", "expected": "2028-02-29"},
            {"tag": "not a leap year", "input": "2026-02-29", "expected": ""},
            {"tag": "february 30", "input": "2026-02-30", "expected": ""},
            {"tag": "month 13", "input": "2026-13-01", "expected": ""},
            {"tag": "day 0", "input": "2026-01-00", "expected": ""},
            {"tag": "day first", "input": "05-09-2026", "expected": ""},
            {"tag": "words", "input": "tomorrow", "expected": ""},
            {"tag": "empty", "input": "", "expected": ""}
        ]
    }

    function test_date_parse(data) {
        compare(DateEntry.parseDate(data.input), data.expected)
    }

    function test_minute_pattern_drops_seconds() {
        compare(DateEntry.minutePattern("HH:mm:ss"), "HH:mm")
        compare(DateEntry.minutePattern("h:mm:ss AP"), "h:mm AP")
        compare(DateEntry.minutePattern("h:mm AP"), "h:mm AP")
        compare(DateEntry.minutePattern("HH.mm"), "HH.mm")
    }

    function test_grid_start_follows_first_day_of_week() {
        // 1 September 2026 is a Tuesday.
        compare(DateEntry.formatIsoDate(DateEntry.gridStart(2026, 8, 1)), "2026-08-31")
        compare(DateEntry.formatIsoDate(DateEntry.gridStart(2026, 8, 7)), "2026-08-30")
        compare(DateEntry.formatIsoDate(DateEntry.gridStart(2026, 8, 6)), "2026-08-29")
        compare(DateEntry.formatIsoDate(DateEntry.gridStart(2026, 8, 2)), "2026-09-01",
                "a month that starts on the first weekday has no leading days")
        compare(DateEntry.weekDayOrder(1), [1, 2, 3, 4, 5, 6, 7])
        compare(DateEntry.weekDayOrder(7), [7, 1, 2, 3, 4, 5, 6])
        compare(DateEntry.weekDayOrder(6), [6, 7, 1, 2, 3, 4, 5])
    }

    function test_add_months_clamps_the_day() {
        compare(DateEntry.formatIsoDate(DateEntry.addMonths(new Date(2026, 0, 31), 1)),
                "2026-02-28")
        compare(DateEntry.formatIsoDate(DateEntry.addMonths(new Date(2026, 0, 15), -1)),
                "2025-12-15")
    }
}
