import QtQuick
import QtTest
import "../../src/app/qml/RecurrenceRule.js" as RecurrenceRule

TestCase {
    name: "RecurrenceRule"

    // Monday 2026-08-17; the third Monday of the month.
    readonly property date start: new Date(2026, 7, 17, 9, 0, 0)
    // Monday 2026-08-31; the fifth and last Monday of the month.
    readonly property date lastMonday: new Date(2026, 7, 31, 9, 0, 0)

    function roundTrip(rule, startDate) {
        const spec = RecurrenceRule.parse(rule, startDate || start)
        verify(spec.representable, rule + " is representable")
        return RecurrenceRule.build(spec, startDate || start)
    }

    function test_common_rules_round_trip_unchanged() {
        compare(roundTrip("FREQ=DAILY"), "FREQ=DAILY")
        compare(roundTrip("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE"),
                "FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE")
        compare(roundTrip("FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=12"),
                "FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=12")
        compare(roundTrip("FREQ=MONTHLY;BYDAY=3MO;UNTIL=20261231T225900Z"),
                "FREQ=MONTHLY;BYDAY=3MO;UNTIL=20261231T225900Z")
        compare(roundTrip("FREQ=MONTHLY;BYDAY=-1MO", lastMonday),
                "FREQ=MONTHLY;BYDAY=-1MO")
        compare(roundTrip("FREQ=YEARLY;INTERVAL=4;UNTIL=20400101"),
                "FREQ=YEARLY;INTERVAL=4;UNTIL=20400101")
        compare(roundTrip("RRULE:FREQ=DAILY;COUNT=3"), "FREQ=DAILY;COUNT=3")
    }

    function test_redundant_parts_normalize() {
        // BYDAY equal to the start's weekday and BYMONTHDAY equal to its day
        // are implied by DTSTART.
        compare(roundTrip("FREQ=WEEKLY;BYDAY=MO"), "FREQ=WEEKLY")
        compare(roundTrip("FREQ=MONTHLY;BYMONTHDAY=17"), "FREQ=MONTHLY")
        compare(roundTrip("FREQ=WEEKLY;INTERVAL=1;WKST=MO"), "FREQ=WEEKLY")
    }

    function test_unsupported_rules_are_not_representable() {
        const rules = ["", "FREQ=HOURLY", "FREQ=MONTHLY;BYSETPOS=-1;BYDAY=MO,TU",
                       "FREQ=MONTHLY;BYMONTHDAY=3", "FREQ=MONTHLY;BYDAY=2TU",
                       "FREQ=YEARLY;BYMONTH=3", "FREQ=WEEKLY;WKST=SU",
                       "FREQ=DAILY;COUNT=2;UNTIL=20270101", "FREQ=DAILY;INTERVAL=0",
                       "FREQ=DAILY\nRDATE:20260901", "FREQ=WEEKLY;BYDAY=XX"]
        for (let index = 0; index < rules.length; ++index)
            verify(!RecurrenceRule.parse(rules[index], start).representable, rules[index])
    }

    function test_descriptions_read_naturally() {
        compare(RecurrenceRule.describe(RecurrenceRule.parse("FREQ=DAILY", start), start, ""),
                "Every day")
        compare(RecurrenceRule.describe(
                    RecurrenceRule.parse("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE", start),
                    start, ""),
                "Every 2 weeks on Monday and Wednesday")
        compare(RecurrenceRule.describe(
                    RecurrenceRule.parse("FREQ=MONTHLY;BYDAY=3MO;COUNT=5", start), start, ""),
                "Every month on the third Monday, 5 times")
        compare(RecurrenceRule.describe(
                    RecurrenceRule.parse("FREQ=MONTHLY;UNTIL=20261231", start), start,
                    "2026-12-31"),
                "Every month on day 17, until 2026-12-31")
    }
}
