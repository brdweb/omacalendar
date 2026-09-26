import QtQuick
import QtTest
import OmaCalendar
import "../../src/app/qml/views" as Views

// Desktop view render benchmark. Each measurement prints one line starting
// with OMACALENDAR_BENCH followed by JSON; scripts/performance/ui_benchmark.py
// runs this file and turns those lines into a report. Timings are reported,
// never asserted, because shared CI workers are too noisy to gate on.
Item {
    id: scene
    width: 1280
    height: 800

    readonly property date referenceDate: new Date(2026, 7, 17, 12, 0, 0)

    Component { id: agendaFactory; Views.AgendaView {} }
    Component { id: dayFactory; Views.DayView {} }
    Component { id: weekFactory; Views.WeekView {} }
    Component { id: monthFactory; Views.MonthView {} }
    Component { id: yearFactory; Views.YearView {} }

    TestCase {
        id: bench
        name: "ViewBenchmark"
        when: windowShown

        readonly property int samples: 3

        // Select a size with qmltestrunner's function:tag syntax, for example
        // ViewBenchmark::test_view_render_and_update:5000.
        function eventCountRows() {
            return [{"tag": "500", "count": 500},
                    {"tag": "5000", "count": 5000},
                    {"tag": "50000", "count": 50000}]
        }

        function test_view_render_and_update_data() {
            return eventCountRows()
        }

        function test_all_views_live_update_data() {
            return eventCountRows()
        }

        function views() {
            return [
                {"name": "agenda", "factory": agendaFactory},
                {"name": "day", "factory": dayFactory},
                {"name": "week", "factory": weekFactory},
                {"name": "month", "factory": monthFactory},
                {"name": "year", "factory": yearFactory}
            ]
        }

        function pad(value) {
            return value < 10 ? "0" + value : String(value)
        }

        function localText(dateValue) {
            return dateValue.getUTCFullYear() + "-" + pad(dateValue.getUTCMonth() + 1)
                    + "-" + pad(dateValue.getUTCDate()) + "T"
                    + pad(dateValue.getUTCHours()) + ":"
                    + pad(dateValue.getUTCMinutes()) + ":00"
        }

        function dateText(dateValue) {
            return localText(dateValue).slice(0, 10)
        }

        // Deterministic events spread over the year around the reference
        // date: mostly timed, some all-day and some multi-day.
        function syntheticEvents(count) {
            const events = []
            const firstDay = Date.UTC(2026, 1, 17)
            const dayMs = 24 * 60 * 60 * 1000
            let seed = 17
            const next = function(limit) {
                seed = (seed * 1103515245 + 12345) % 2147483648
                return seed % limit
            }
            for (let index = 0; index < count; ++index) {
                const day = next(365)
                const kind = next(20)
                const value = {
                    "id": "bench-" + index,
                    "calendarId": "calendar-writable",
                    "summary": "Benchmark event " + index,
                    "description": "",
                    "location": "",
                    "calendarColor": "#7aa2f7",
                    "visibility": "default",
                    "transparency": "opaque",
                    "organizer": ({}),
                    "attendees": [],
                    "reminders": [],
                    "localRevision": index + 1
                }
                if (kind < 2) {
                    const start = new Date(firstDay + day * dayMs)
                    const end = new Date(start.getTime() + (1 + next(3)) * dayMs)
                    value.allDay = true
                    value.timeKind = "all-day"
                    value.startDate = dateText(start)
                    value.endDate = dateText(end)
                } else {
                    const start = new Date(firstDay + day * dayMs
                                           + (6 + next(14)) * 60 * 60 * 1000
                                           + next(4) * 15 * 60 * 1000)
                    const minutes = kind === 2 ? 36 * 60 : 30 + next(4) * 30
                    const end = new Date(start.getTime() + minutes * 60 * 1000)
                    value.allDay = false
                    value.timeKind = "zoned"
                    value.startUtc = start.toISOString()
                    value.endUtc = end.toISOString()
                    value.displayStartLocal = localText(start)
                    value.displayEndLocal = localText(end)
                    value.startTimeZone = "UTC"
                    value.endTimeZone = "UTC"
                }
                events.push(value)
            }
            return events
        }

        // A copy of the list with one event edited, as the controller
        // delivers after a single change.
        function withOneChange(events, generation) {
            const changed = events.slice()
            const index = Math.floor(changed.length / 2)
            const edited = Object.assign({}, changed[index])
            edited.summary = "Edited " + generation
            edited.localRevision = changed[index].localRevision + generation
            changed[index] = edited
            return changed
        }

        function median(values) {
            const sorted = values.slice().sort(function(first, second) {
                return first - second
            })
            return sorted[Math.floor(sorted.length / 2)]
        }

        function report(view, operation, eventCount, values) {
            console.log("OMACALENDAR_BENCH " + JSON.stringify({
                "view": view,
                "operation": operation,
                "events": eventCount,
                "samplesMs": values,
                "medianMs": median(values)
            }))
        }

        function createView(factory, events) {
            const object = createTemporaryObject(factory, scene, {
                "width": scene.width,
                "height": scene.height,
                "currentDate": scene.referenceDate,
                "events": events,
                "visible": true
            })
            verify(object !== null, "benchmark view created")
            return object
        }

        function settle(item) {
            if (isPolishScheduled(item))
                waitForItemPolished(item)
        }

        function test_view_render_and_update(row) {
            const events = syntheticEvents(row.count)
            const viewList = views()
            for (let viewIndex = 0; viewIndex < viewList.length; ++viewIndex) {
                const view = viewList[viewIndex]
                const renders = []
                const updates = []
                for (let sample = 0; sample < samples; ++sample) {
                    let started = Date.now()
                    const object = createView(view.factory, events)
                    settle(object)
                    renders.push(Date.now() - started)

                    started = Date.now()
                    object.events = withOneChange(events, sample + 1)
                    settle(object)
                    updates.push(Date.now() - started)
                    object.destroy()
                    wait(0)
                }
                report(view.name, "render", row.count, renders)
                report(view.name, "update", row.count, updates)
            }
        }

        // Main.qml's view stack keeps every view alive and bound to the same
        // events, so one change reaches all five. This measures that cost next
        // to the single-view numbers above.
        function test_all_views_live_update(row) {
            const events = syntheticEvents(row.count)
            const viewList = views()
            const objects = []
            for (let viewIndex = 0; viewIndex < viewList.length; ++viewIndex)
                objects.push(createView(viewList[viewIndex].factory, events))
            wait(0)
            const updates = []
            for (let sample = 0; sample < samples; ++sample) {
                const changed = withOneChange(events, sample + 1)
                const started = Date.now()
                for (let index = 0; index < objects.length; ++index)
                    objects[index].events = changed
                for (let index = 0; index < objects.length; ++index)
                    settle(objects[index])
                updates.push(Date.now() - started)
            }
            report("all-views", "update", row.count, updates)
            for (let index = 0; index < objects.length; ++index)
                objects[index].destroy()
            wait(0)
        }
    }
}
