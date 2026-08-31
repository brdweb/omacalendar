import QtQuick
import QtQuick.Window
import QtTest
import "../../src/app/qml/views" as Views
import "../../src/app/qml/components" as Components

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
    Component { id: eventChipFactory; Components.EventChip {} }
    Component { id: timelineCanvasFactory; Components.TimelineCanvas {} }
    Component { id: timelineEventFactory; Components.TimelineEvent {} }
    Component { id: editorFactory; Components.EventEditor {} }
    Component { id: mutationConfirmationFactory; Components.MutationConfirmationDialog {} }
    Component { id: activityFactory; Components.ActivityPanel {} }
    Component { id: settingsFactory; Components.AccountSettingsDrawer {} }

    SignalSpy {
        id: activationSpy
        signalName: "activated"
    }

    SignalSpy {
        id: creationSpy
        signalName: "createRequested"
    }

    SignalSpy {
        id: rescheduleSpy
        signalName: "rescheduleRequested"
    }

    SignalSpy {
        id: dateChangeSpy
        signalName: "eventDateChanged"
    }

    SignalSpy {
        id: editorSaveSpy
        signalName: "saveRequested"
    }

    SignalSpy {
        id: mutationConfirmedSpy
        signalName: "confirmed"
    }

    SignalSpy {
        id: invitationResponseSpy
        signalName: "invitationResponseRequested"
    }

    SignalSpy {
        id: invitationSeenSpy
        signalName: "invitationSeenRequested"
    }

    SignalSpy {
        id: localCalendarRemovalSpy
        signalName: "removeCalendarRequested"
    }

    SignalSpy {
        id: preferenceChangedSpy
        signalName: "preferenceChanged"
    }

    SignalSpy {
        id: calendarPreferenceSpy
        signalName: "calendarPreferenceChanged"
    }

    TestCase {
        id: testCase
        name: "DesktopSmoke"
        when: windowShown

        function init() {
            // Any QML warning produced during a test is a failure. Component
            // load errors already fail qmltestrunner before a test starts.
            failOnWarning(/.*/)
        }

        function viewportProfiles() {
            return [
                {"name": "100-percent", "factor": 1.0,
                 "width": 980, "height": 660},
                {"name": "125-percent", "factor": 1.25,
                 "width": 1225, "height": 825},
                {"name": "200-percent", "factor": 2.0,
                 "width": 1960, "height": 1320}
            ]
        }

        function representativeEvents() {
            const events = [
                {
                    "id": "event-timed",
                    "calendarId": "calendar-writable",
                    "summary": "Design review",
                    "description": "Representative timed event",
                    "location": "Studio",
                    "calendarColor": "#7aa2f7",
                    "allDay": false,
                    "timeKind": "zoned",
                    "startUtc": "2026-08-17T13:00:00.000Z",
                    "endUtc": "2026-08-17T14:00:00.000Z",
                    "displayStartLocal": "2026-08-17T09:00:00",
                    "displayEndLocal": "2026-08-17T10:00:00",
                    "startTimeZone": "America/New_York",
                    "endTimeZone": "America/New_York",
                    "visibility": "default",
                    "transparency": "opaque",
                    "organizer": {"displayName": "Avery", "email": "avery@example.com"},
                    "attendees": [{"email": "me@example.com", "partstat": "ACCEPTED"}],
                    "reminders": [{"method": "popup", "minutes": 15}]
                },
                {
                    "id": "event-all-day",
                    "calendarId": "calendar-writable",
                    "summary": "Release day",
                    "calendarColor": "#bb9af7",
                    "allDay": true,
                    "timeKind": "all-day",
                    "startDate": "2026-08-17",
                    "endDate": "2026-08-18",
                    "visibility": "public",
                    "transparency": "transparent"
                },
                {
                    "id": "event-multi-day",
                    "calendarId": "calendar-writable",
                    "summary": "Conference",
                    "calendarColor": "#e0af68",
                    "allDay": false,
                    "timeKind": "zoned",
                    "startUtc": "2026-08-17T02:00:00.000Z",
                    "endUtc": "2026-08-18T12:00:00.000Z",
                    "displayStartLocal": "2026-08-16T22:00:00",
                    "displayEndLocal": "2026-08-18T08:00:00",
                    "startTimeZone": "America/New_York",
                    "endTimeZone": "America/New_York"
                },
                {
                    "id": "event-pending",
                    "calendarId": "calendar-writable",
                    "summary": "Pending edit",
                    "calendarColor": "#73daca",
                    "allDay": false,
                    "timeKind": "floating",
                    "startUtc": "2026-08-17T11:00:00.000Z",
                    "endUtc": "2026-08-17T11:45:00.000Z",
                    "displayStartLocal": "2026-08-17T11:00:00",
                    "displayEndLocal": "2026-08-17T11:45:00",
                    "dirty": true,
                    "operationState": "pending"
                },
                {
                    "id": "event-read-only",
                    "calendarId": "calendar-read-only",
                    "summary": "Subscribed event",
                    "calendarColor": "#9ece6a",
                    "allDay": false,
                    "timeKind": "zoned",
                    "startUtc": "2026-08-17T19:00:00.000Z",
                    "endUtc": "2026-08-17T20:00:00.000Z",
                    "displayStartLocal": "2026-08-17T15:00:00",
                    "displayEndLocal": "2026-08-17T16:00:00",
                    "readOnly": true
                },
                {
                    "id": "event-conflict",
                    "calendarId": "calendar-writable",
                    "summary": "Conflicting edit",
                    "calendarColor": "#f7768e",
                    "allDay": false,
                    "timeKind": "zoned",
                    "startUtc": "2026-08-17T21:00:00.000Z",
                    "endUtc": "2026-08-17T22:00:00.000Z",
                    "displayStartLocal": "2026-08-17T17:00:00",
                    "displayEndLocal": "2026-08-17T18:00:00",
                    "conflict": true,
                    "operationState": "blocked"
                }
            ]
            // Presentation DTOs always provide object/array fields even when
            // provider metadata is absent. Keep the fixture faithful to that
            // contract so warnings indicate a UI defect rather than malformed
            // test input.
            for (let index = 0; index < events.length; ++index) {
                events[index].localRevision = index + 1
                if (events[index].organizer === undefined)
                    events[index].organizer = ({})
                if (events[index].attendees === undefined)
                    events[index].attendees = []
                if (events[index].reminders === undefined)
                    events[index].reminders = []
            }
            return events
        }

        function verifyFiniteGeometry(item, label) {
            verify(item !== null, label + " exists")
            verify(isFinite(Number(item.x)), label + " has a finite x")
            verify(isFinite(Number(item.y)), label + " has a finite y")
            verify(isFinite(Number(item.width)),
                   label + " has a finite width (" + item.width + ")")
            verify(isFinite(Number(item.height)),
                   label + " has a finite height (" + item.height + ")")
            const visualChildren = item.children || []
            for (let index = 0; index < visualChildren.length; ++index) {
                const child = visualChildren[index]
                if (child && child.x !== undefined && child.width !== undefined)
                    verifyFiniteGeometry(child, label + "/child-" + index)
            }
        }

        function createView(factory, profile, events) {
            const object = createTemporaryObject(factory, scene, {
                "width": profile.width,
                "height": profile.height,
                "currentDate": scene.referenceDate,
                "events": events,
                "visible": true
            })
            verify(object !== null, "view created at " + profile.name)
            wait(0)
            compare(object.width, profile.width)
            compare(object.height, profile.height)
            verifyFiniteGeometry(object, profile.name)
            return object
        }

        function dragItemTo(sourceItem, targetItem, coordinateRoot) {
            const sourcePoint = sourceItem.mapToItem(
                                      coordinateRoot, sourceItem.width / 2,
                                      sourceItem.height / 2)
            const targetPoint = targetItem.mapToItem(
                                      coordinateRoot, targetItem.width / 2,
                                      targetItem.height / 2)
            mousePress(coordinateRoot, sourcePoint.x, sourcePoint.y, Qt.LeftButton)
            mouseMove(coordinateRoot, (sourcePoint.x + targetPoint.x) / 2,
                      (sourcePoint.y + targetPoint.y) / 2, 10, Qt.LeftButton)
            mouseMove(coordinateRoot, targetPoint.x, targetPoint.y, 10,
                      Qt.LeftButton)
            mouseRelease(coordinateRoot, targetPoint.x, targetPoint.y,
                         Qt.LeftButton)
        }

        function test_all_views_empty_and_representative() {
            const profiles = viewportProfiles()
            const events = representativeEvents()
            const factories = [agendaFactory, dayFactory, weekFactory,
                               monthFactory, yearFactory]
            for (let profileIndex = 0; profileIndex < profiles.length;
                 ++profileIndex) {
                const profile = profiles[profileIndex]
                for (let factoryIndex = 0; factoryIndex < factories.length;
                     ++factoryIndex) {
                    const emptyView = createView(factories[factoryIndex], profile, [])
                    emptyView.destroy()
                    wait(0)

                    const populatedView = createView(factories[factoryIndex], profile,
                                                     events)
                    if (factoryIndex === 0)
                        compare(populatedView.eventsForDate(scene.referenceDate).length, 6)
                    else if (factoryIndex === 1) {
                        compare(populatedView.allDayEvents.length, 1)
                        compare(populatedView.spanningEvents.length, 1)
                        compare(populatedView.headerEvents.length, 2)
                        compare(populatedView.timedEvents.length, 4)
                    } else if (factoryIndex === 2) {
                        compare(populatedView.eventsForDate(scene.referenceDate, true).length,
                                2)
                        compare(populatedView.eventsForDate(scene.referenceDate, false).length,
                                4)
                    } else if (factoryIndex === 3)
                        compare(populatedView.eventsForDate(scene.referenceDate).length, 6)
                    else
                        // YearView intentionally marks the start date only for
                        // timed multi-day events; the continuation is covered
                        // by the agenda/month assertions above.
                        compare(populatedView.eventCount(scene.referenceDate), 5)
                    populatedView.destroy()
                    wait(0)
                }
            }
        }

        function test_event_states_and_keyboard_activation() {
            const events = representativeEvents()
            const failedEvent = Object.assign({}, events[0], {
                "id": "event-failed", "operationState": "failed"
            })
            const expectedStates = [
                {"event": events[3], "label": "Pending"},
                {"event": events[4], "label": "Read only"},
                {"event": events[5], "label": "Conflict"},
                {"event": failedEvent, "label": "Failed"}
            ]
            for (let index = 0; index < expectedStates.length; ++index) {
                const chip = createTemporaryObject(eventChipFactory, scene, {
                    "eventData": expectedStates[index].event,
                    "width": 360,
                    "height": 40,
                    "compact": false
                })
                verify(chip !== null)
                compare(chip.stateText, expectedStates[index].label)
                verifyFiniteGeometry(chip, "event-state-" + index)
                chip.destroy()
                wait(0)
            }

            const timelineEvent = createTemporaryObject(timelineEventFactory, scene, {
                "eventData": events[0],
                "width": 320,
                "height": 64,
                "startMinute": 540,
                "durationMinutes": 60
            })
            verify(timelineEvent !== null)
            activationSpy.target = timelineEvent
            activationSpy.clear()
            timelineEvent.forceActiveFocus()
            tryCompare(timelineEvent, "activeFocus", true)
            keyClick(Qt.Key_Return)
            compare(activationSpy.count, 1)
            activationSpy.target = null
            timelineEvent.destroy()
            wait(0)

            const failedTimeline = createTemporaryObject(timelineEventFactory, scene, {
                "eventData": failedEvent,
                "width": 320,
                "height": 64,
                "startMinute": 540,
                "durationMinutes": 60
            })
            verify(failedTimeline !== null)
            compare(failedTimeline.stateText, "Failed")
            verify(!failedTimeline.editable)
            failedTimeline.destroy()
            wait(0)
        }

        function test_month_move_guards_and_week_target_date() {
            const events = representativeEvents()
            const profile = viewportProfiles()[0]
            const month = createView(monthFactory, profile, events)
            dateChangeSpy.target = month
            dateChangeSpy.clear()

            verify(month.requestDateChange(events[0], new Date(2026, 7, 18)))
            compare(dateChangeSpy.count, 1)
            compare(dateChangeSpy.signalArguments[0][0].id, "event-timed")
            compare(Qt.formatDate(dateChangeSpy.signalArguments[0][1], "yyyy-MM-dd"),
                    "2026-08-18")
            verify(!month.requestDateChange(events[0], new Date(2026, 7, 17)))
            verify(!month.requestDateChange(events[4], new Date(2026, 7, 18)))
            verify(!month.requestDateChange(events[3], new Date(2026, 7, 18)))
            compare(dateChangeSpy.count, 1)

            dateChangeSpy.clear()
            const sourceChip = findChild(month, "monthEvent-21-event-all-day")
            const targetArea = findChild(month, "monthDropArea-22")
            verify(sourceChip !== null, "month drag source exists")
            verify(targetArea !== null, "month drop target exists")
            verify(sourceChip.draggable)
            const sourcePoint = sourceChip.mapToItem(
                                      month, sourceChip.width / 2,
                                      sourceChip.height / 2)
            const targetPoint = targetArea.mapToItem(
                                      month, targetArea.width / 2,
                                      targetArea.height / 2)
            mousePress(month, sourcePoint.x, sourcePoint.y, Qt.LeftButton)
            mouseMove(month, (sourcePoint.x + targetPoint.x) / 2,
                      (sourcePoint.y + targetPoint.y) / 2, 10, Qt.LeftButton)
            mouseMove(month, targetPoint.x, targetPoint.y, 10, Qt.LeftButton)
            mouseRelease(month, targetPoint.x, targetPoint.y, Qt.LeftButton)
            tryCompare(dateChangeSpy, "count", 1)
            compare(dateChangeSpy.signalArguments[0][0].id, "event-all-day")
            compare(Qt.formatDate(dateChangeSpy.signalArguments[0][1], "yyyy-MM-dd"),
                    "2026-08-18")

            dateChangeSpy.clear()
            const moreButton = findChild(month, "monthMore-21")
            verify(moreButton !== null, "month overflow button exists")
            moreButton.forceActiveFocus()
            tryCompare(moreButton, "activeFocus", true)
            keyClick(Qt.Key_Return)
            tryVerify(function() {
                return month.activeOverflowPopup !== null
            })
            const overflowPopup = month.activeOverflowPopup
            verify(overflowPopup !== null, "month overflow popup exists")
            tryCompare(overflowPopup, "opened", true)
            const overflowEvent = findChild(
                                      overflowPopup.contentItem,
                                      "monthOverflowEvent-21-event-timed")
            const previousDayButton = findChild(
                                          overflowPopup.contentItem,
                                          "monthOverflowPrevious-21-event-timed")
            verify(overflowEvent !== null, "month overflow event exists")
            verify(previousDayButton !== null,
                   "month overflow reschedule control exists")
            verify(overflowEvent.draggable)
            previousDayButton.forceActiveFocus()
            tryCompare(previousDayButton, "activeFocus", true)
            keyClick(Qt.Key_Return)
            tryCompare(dateChangeSpy, "count", 1)
            compare(dateChangeSpy.signalArguments[0][0].id, "event-timed")
            compare(Qt.formatDate(dateChangeSpy.signalArguments[0][1], "yyyy-MM-dd"),
                    "2026-08-16")
            overflowPopup.close()
            tryCompare(overflowPopup, "opened", false)

            dateChangeSpy.target = null
            month.destroy()
            wait(0)

            const week = createView(weekFactory, profile, events)
            compare(Qt.formatDate(week.targetDateForMove(0, 1), "yyyy-MM-dd"),
                    "2026-08-18")
            compare(Qt.formatDate(week.targetDateForMove(6, 2), "yyyy-MM-dd"),
                    "2026-08-25")
            compare(Qt.formatDate(week.targetDateForMove(0, -2), "yyyy-MM-dd"),
                    "2026-08-15")
            const weekScroll = findChild(week, "weekTimelineScroll")
            const weekAllDayLane = findChild(week, "weekAllDayLane")
            verify(weekScroll !== null, "week timeline scroll surface exists")
            verify(weekAllDayLane !== null, "week all-day lane exists")
            compare(weekAllDayLane.y, 55)
            compare(weekAllDayLane.height, 66)
            compare(weekScroll.y,
                    weekAllDayLane.y + weekAllDayLane.height)
            verify(weekScroll.height > 250,
                   "week timeline receives the remaining viewport height")
            verify(weekScroll.contentHeight > weekScroll.height,
                   "week timeline has vertical overflow")
            const firstWeekHeader = findChild(week, "weekDayHeader-0")
            const firstWeekAllDay = findChild(week, "weekAllDayColumn-0")
            const firstWeekTimeline = findChild(week, "weekTimelineDay-0")
            verify(firstWeekHeader !== null && firstWeekAllDay !== null
                   && firstWeekTimeline !== null)
            const headerPoint = firstWeekHeader.mapToItem(week, 0, 0)
            const allDayPoint = firstWeekAllDay.mapToItem(week, 0, 0)
            const timelinePoint = firstWeekTimeline.mapToItem(week, 0, 0)
            fuzzyCompare(headerPoint.x, allDayPoint.x, 0.5)
            fuzzyCompare(headerPoint.x, timelinePoint.x, 0.5)
            fuzzyCompare(firstWeekHeader.width, firstWeekAllDay.width, 0.5)
            fuzzyCompare(firstWeekHeader.width, firstWeekTimeline.width, 0.5)
            verify(headerPoint.x + firstWeekHeader.width * 7
                   <= week.width - 15,
                   "week grid reserves the right-side gutter")
            const initialWeekScroll = weekScroll.contentY
            mouseWheel(weekScroll, weekScroll.width / 2, weekScroll.height / 2,
                       0, -120)
            tryVerify(function() {
                return weekScroll.contentY > initialWeekScroll
            }, 1000, "mouse wheel scrolls the week timeline")
            week.destroy()
            wait(0)

            const year = createView(yearFactory, profile, events)
            const firstMonthCard = findChild(year, "yearMonthCard-0")
            verify(firstMonthCard !== null, "year month card exists")
            compare(firstMonthCard.height, 282)
            verify(firstMonthCard.clip,
                   "year month card clips calendar content to its bounds")
            year.destroy()
            wait(0)
        }

        function test_day_and_week_header_drag_rescheduling() {
            const events = representativeEvents()
            const profile = viewportProfiles()[0]
            const day = createView(dayFactory, profile, events)
            dateChangeSpy.target = day
            dateChangeSpy.clear()

            const dayAllDay = findChild(day, "dayHeaderEvent-event-all-day")
            const previousDay = findChild(day, "dayHeaderPreviousDrop")
            verify(dayAllDay !== null, "day all-day drag source exists")
            verify(previousDay !== null, "previous-day drop target exists")
            verify(dayAllDay.draggable)
            dragItemTo(dayAllDay, previousDay, day)
            tryCompare(dateChangeSpy, "count", 1)
            compare(dateChangeSpy.signalArguments[0][0].id, "event-all-day")
            compare(Qt.formatDate(dateChangeSpy.signalArguments[0][1], "yyyy-MM-dd"),
                    "2026-08-16")

            dateChangeSpy.clear()
            const daySpanning = findChild(day, "dayHeaderEvent-event-multi-day")
            const nextDay = findChild(day, "dayHeaderNextDrop")
            verify(daySpanning !== null, "day spanning drag source exists")
            verify(nextDay !== null, "next-day drop target exists")
            verify(daySpanning.draggable)
            dragItemTo(daySpanning, nextDay, day)
            tryCompare(dateChangeSpy, "count", 1)
            compare(dateChangeSpy.signalArguments[0][0].id, "event-multi-day")
            compare(Qt.formatDate(dateChangeSpy.signalArguments[0][1], "yyyy-MM-dd"),
                    "2026-08-18")

            dateChangeSpy.target = null
            day.destroy()
            wait(0)

            const week = createView(weekFactory, profile, events)
            dateChangeSpy.target = week
            dateChangeSpy.clear()

            const weekSpanning = findChild(
                                      week,
                                      "weekHeaderEvent-0-event-multi-day")
            const thursdayDrop = findChild(week, "weekHeaderDrop-3")
            verify(weekSpanning !== null, "week spanning drag source exists")
            verify(thursdayDrop !== null, "week day drop target exists")
            verify(weekSpanning.draggable)
            dragItemTo(weekSpanning, thursdayDrop, week)
            tryCompare(dateChangeSpy, "count", 1)
            compare(dateChangeSpy.signalArguments[0][0].id, "event-multi-day")
            compare(Qt.formatDate(dateChangeSpy.signalArguments[0][1], "yyyy-MM-dd"),
                    "2026-08-20")

            dateChangeSpy.clear()
            const weekAllDay = findChild(
                                    week,
                                    "weekHeaderEvent-0-event-all-day")
            const nextWeek = findChild(week, "weekNextDrop")
            verify(weekAllDay !== null, "week all-day drag source exists")
            verify(nextWeek !== null, "next-week edge drop target exists")
            dragItemTo(weekAllDay, nextWeek, week)
            tryCompare(dateChangeSpy, "count", 1)
            compare(dateChangeSpy.signalArguments[0][0].id, "event-all-day")
            compare(Qt.formatDate(dateChangeSpy.signalArguments[0][1], "yyyy-MM-dd"),
                    "2026-08-24")

            dateChangeSpy.target = null
            week.destroy()
            wait(0)
        }

        function test_day_and_week_overlapping_events_share_width() {
            const base = representativeEvents()[0]
            const first = Object.assign({}, base, {
                "id": "overlap-first",
                "summary": "First overlapping event",
                "displayStartLocal": "2026-08-17T09:00:00",
                "displayEndLocal": "2026-08-17T10:30:00"
            })
            const second = Object.assign({}, base, {
                "id": "overlap-second",
                "summary": "Second overlapping event",
                "displayStartLocal": "2026-08-17T09:30:00",
                "displayEndLocal": "2026-08-17T10:00:00"
            })
            const profile = viewportProfiles()[0]
            const day = createView(dayFactory, profile, [first, second])
            const dayFirst = findChild(day, "dayTimedEvent-overlap-first")
            const daySecond = findChild(day, "dayTimedEvent-overlap-second")
            verify(dayFirst !== null && daySecond !== null)
            verify(dayFirst.x !== daySecond.x,
                   "overlapping day events occupy separate columns")
            verify(dayFirst.width < day.width / 2 && daySecond.width < day.width / 2)
            day.destroy()
            wait(0)

            const week = createView(weekFactory, profile, [first, second])
            const weekFirst = findChild(week, "weekTimedEvent-0-overlap-first")
            const weekSecond = findChild(week, "weekTimedEvent-0-overlap-second")
            verify(weekFirst !== null && weekSecond !== null)
            verify(weekFirst.x !== weekSecond.x,
                   "overlapping week events occupy separate columns")
            verify(weekFirst.width < weekFirst.parent.width / 2)
            verify(weekSecond.width < weekSecond.parent.width / 2)
            week.destroy()
            wait(0)
        }

        function test_timeline_click_and_drag_creation() {
            const canvas = createTemporaryObject(timelineCanvasFactory, scene, {
                "width": 300,
                "height": 480,
                "firstHour": 0,
                "lastHour": 24,
                "pixelsPerHour": 20,
                "defaultDurationMinutes": 45
            })
            verify(canvas !== null)
            creationSpy.target = canvas
            creationSpy.clear()

            mouseClick(canvas, 50, 180, Qt.LeftButton)
            compare(creationSpy.count, 1)
            compare(creationSpy.signalArguments[0][0], 540)
            compare(creationSpy.signalArguments[0][1], 45)

            creationSpy.clear()
            mousePress(canvas, 50, 200, Qt.LeftButton)
            mouseMove(canvas, 50, 230, 10, Qt.LeftButton)
            mouseRelease(canvas, 50, 230, Qt.LeftButton)
            compare(creationSpy.count, 1)
            compare(creationSpy.signalArguments[0][0], 600)
            compare(creationSpy.signalArguments[0][1], 90)

            creationSpy.target = null
            canvas.destroy()
            wait(0)
        }

        function test_timeline_move_and_edge_resize() {
            const events = representativeEvents()
            const timelineEvent = createTemporaryObject(timelineEventFactory, scene, {
                "eventData": events[0],
                "width": 100,
                "height": 60,
                "y": 100,
                "startMinute": 540,
                "durationMinutes": 60,
                "pixelsPerHour": 60,
                "horizontalRescheduleEnabled": true,
                "dayWidth": 100
            })
            verify(timelineEvent !== null)
            verify(timelineEvent.editable)
            compare(timelineEvent.stateText, "")
            rescheduleSpy.target = timelineEvent
            rescheduleSpy.clear()

            mousePress(timelineEvent, 50, 30, Qt.LeftButton)
            mouseMove(timelineEvent, 150, 45, 10, Qt.LeftButton)
            mouseRelease(timelineEvent, 150, 45, Qt.LeftButton)
            compare(rescheduleSpy.count, 1)
            compare(rescheduleSpy.signalArguments[0][0].id, "event-timed")
            compare(rescheduleSpy.signalArguments[0][1], 555)
            compare(rescheduleSpy.signalArguments[0][2], 60)
            compare(rescheduleSpy.signalArguments[0][3], 1)

            rescheduleSpy.clear()
            mousePress(timelineEvent, 50, 30, Qt.LeftButton)
            mouseMove(timelineEvent, 850, 30, 10, Qt.LeftButton)
            mouseRelease(timelineEvent, 850, 30, Qt.LeftButton)
            compare(rescheduleSpy.count, 1)
            compare(rescheduleSpy.signalArguments[0][1], 540)
            compare(rescheduleSpy.signalArguments[0][2], 60)
            compare(rescheduleSpy.signalArguments[0][3], 8)

            rescheduleSpy.clear()
            mousePress(timelineEvent, 50, 3, Qt.LeftButton)
            mouseMove(timelineEvent, 50, -12, 10, Qt.LeftButton)
            mouseRelease(timelineEvent, 50, -12, Qt.LeftButton)
            compare(rescheduleSpy.count, 1)
            compare(rescheduleSpy.signalArguments[0][1], 525)
            compare(rescheduleSpy.signalArguments[0][2], 75)
            compare(rescheduleSpy.signalArguments[0][3], 0)

            rescheduleSpy.clear()
            mousePress(timelineEvent, 50, 57, Qt.LeftButton)
            mouseMove(timelineEvent, 50, 87, 10, Qt.LeftButton)
            mouseRelease(timelineEvent, 50, 87, Qt.LeftButton)
            compare(rescheduleSpy.count, 1)
            compare(rescheduleSpy.signalArguments[0][1], 540)
            compare(rescheduleSpy.signalArguments[0][2], 90)

            activationSpy.target = timelineEvent
            activationSpy.clear()
            rescheduleSpy.clear()
            mouseClick(timelineEvent, 50, 30, Qt.LeftButton)
            compare(activationSpy.count, 1)
            compare(rescheduleSpy.count, 0)

            activationSpy.target = null
            rescheduleSpy.target = null
            timelineEvent.destroy()
            wait(0)

            const longEvent = createTemporaryObject(timelineEventFactory, scene, {
                "eventData": events[0],
                "width": 100,
                "height": 60,
                "startMinute": 15,
                "durationMinutes": 1500,
                "pixelsPerHour": 60
            })
            verify(longEvent !== null)
            verify(longEvent.resizable,
                   "events lasting at least 24 hours retain resize handles")
            rescheduleSpy.target = longEvent
            rescheduleSpy.clear()

            mousePress(longEvent, 50, 57, Qt.LeftButton)
            mouseMove(longEvent, 50, 117, 10, Qt.LeftButton)
            mouseRelease(longEvent, 50, 117, Qt.LeftButton)
            compare(rescheduleSpy.count, 1)
            compare(rescheduleSpy.signalArguments[0][1], 15)
            compare(rescheduleSpy.signalArguments[0][2], 1560)
            compare(rescheduleSpy.signalArguments[0][3], 0)

            rescheduleSpy.clear()
            mousePress(longEvent, 50, 3, Qt.LeftButton)
            mouseMove(longEvent, 50, -27, 10, Qt.LeftButton)
            mouseRelease(longEvent, 50, -27, Qt.LeftButton)
            compare(rescheduleSpy.count, 1)
            compare(rescheduleSpy.signalArguments[0][1], 1425)
            compare(rescheduleSpy.signalArguments[0][2], 1530)
            compare(rescheduleSpy.signalArguments[0][3], -1)

            rescheduleSpy.target = null
            longEvent.destroy()
            wait(0)

            const pendingEvent = createTemporaryObject(timelineEventFactory, scene, {
                "eventData": events[3],
                "width": 100,
                "height": 60
            })
            verify(pendingEvent !== null)
            compare(pendingEvent.stateText, "Pending")
            verify(!pendingEvent.editable)
            pendingEvent.destroy()
            wait(0)

            const readOnlyEvent = createTemporaryObject(timelineEventFactory, scene, {
                "eventData": events[4],
                "width": 100,
                "height": 60
            })
            verify(readOnlyEvent !== null)
            compare(readOnlyEvent.stateText, "Read only")
            verify(!readOnlyEvent.editable)
            readOnlyEvent.destroy()
            wait(0)
        }

        function test_editor_activity_and_settings_surfaces() {
            const events = representativeEvents()
            const editor = createTemporaryObject(editorFactory, scene)
            verify(editor !== null)
            editor.openNew(scene.referenceDate, 540)
            tryCompare(editor, "opened", true)
            verify(scene.Window.window.activeFocusItem !== null,
                   "new-event editor establishes keyboard focus")
            keyClick(Qt.Key_Escape)
            tryCompare(editor, "opened", false)

            editor.openExisting(events[4])
            tryCompare(editor, "opened", true)
            verify(editor.editing)
            verify(editor.readOnly)
            verifyFiniteGeometry(editor.contentItem, "read-only-editor")
            editor.close()
            tryCompare(editor, "opened", false)

            const inheritedReadOnly = Object.assign({}, events[4])
            delete inheritedReadOnly.readOnly
            editor.openExisting(inheritedReadOnly)
            tryCompare(editor, "opened", true)
            verify(editor.readOnly,
                   "calendar read-only capability protects raw search/invitation DTOs")
            editor.close()
            tryCompare(editor, "opened", false)

            const recurring = Object.assign({}, events[0], {
                "recurrenceRule": "FREQ=WEEKLY",
                "calendarId": "calendar-writable"
            })
            editor.openExisting(recurring)
            tryCompare(editor, "opened", true)
            verify(!editor.futureScopeSupported,
                   "local calendars do not imply this-and-future capability")
            let recurrenceScope = findChild(scene.Window.window.contentItem,
                                             "eventRecurrenceScope")
            verify(recurrenceScope !== null)
            compare(recurrenceScope.count, 3)
            editor.close()
            tryCompare(editor, "opened", false)

            const futureCapable = Object.assign({}, recurring, {
                "calendarId": "calendar-future-scope"
            })
            editor.openExisting(futureCapable)
            tryCompare(editor, "opened", true)
            verify(editor.futureScopeSupported)
            recurrenceScope = findChild(scene.Window.window.contentItem,
                                        "eventRecurrenceScope")
            compare(recurrenceScope.count, 4)
            const calendarSelector = findChild(scene.Window.window.contentItem,
                                               "eventCalendar")
            verify(calendarSelector !== null)
            calendarSelector.currentIndex = 0
            calendarSelector.activated(0)
            wait(0)
            verify(editor.movingCalendars)
            verify(!editor.futureScopeSupported,
                   "calendar moves never expose an unsupported future scope")
            compare(recurrenceScope.count, 3)
            editor.close()
            tryCompare(editor, "opened", false)
            editor.destroy()
            wait(0)

            const activity = createTemporaryObject(activityFactory, scene, {
                "mode": "search",
                "searchResults": events,
                "invitations": [{
                    "id": "invite-1",
                    "calendarId": "calendar-google",
                    "summary": "Planning invitation",
                    "startUtc": "2026-08-17T13:00:00.000Z",
                    "recurrenceRule": "FREQ=WEEKLY",
                    "recurrenceId": "2026-08-17T13:00:00.000Z",
                    "localRevision": 7,
                    "seen": false,
                    "organizer": {"displayName": "Morgan"}
                }],
                "conflicts": [{
                    "id": "conflict-1",
                    "summary": "Conflicting edit",
                    "message": "Local and remote copies changed"
                }],
                "operations": [
                    {"id": 1, "operation": "update", "state": "retry_wait",
                     "errorMessage": "Temporary service error"},
                    {"id": 2, "operation": "remove", "state": "blocked",
                     "errorMessage": "Resolve the conflict first"}
                ],
                "calendars": [{
                    "id": "calendar-google", "accountId": "account-google",
                    "name": "Google", "readOnly": false,
                    "capabilities": {"provider": "google"}
                }],
                "accounts": [{
                    "id": "account-google", "provider": "google"
                }],
                "connected": false,
                "statusText": "Cached data available"
            })
            verify(activity !== null)
            activity.open()
            tryCompare(activity, "opened", true)
            for (const mode of ["search", "invitations"]) {
                activity.mode = mode
                wait(0)
                compare(activity.mode, mode)
                verifyFiniteGeometry(activity.contentItem, "activity-" + mode)
            }
            invitationResponseSpy.target = activity
            invitationSeenSpy.target = activity
            invitationResponseSpy.clear()
            invitationSeenSpy.clear()
            activity.requestInvitationResponse(activity.invitations[0], "accepted")
            compare(invitationSeenSpy.count, 1)
            const invitationScope = findChild(scene.Window.window.contentItem,
                                               "invitationRecurrenceScope")
            const invitationConfirm = findChild(scene.Window.window.contentItem,
                                                 "confirmInvitationResponse")
            verify(invitationScope !== null)
            verify(invitationConfirm !== null)
            verify(!invitationConfirm.enabled)
            invitationScope.currentIndex = 1
            invitationScope.activated(1)
            wait(0)
            verify(invitationConfirm.enabled)
            invitationConfirm.clicked()
            compare(invitationResponseSpy.count, 1)
            compare(invitationResponseSpy.signalArguments[0][0], "invite-1")
            compare(invitationResponseSpy.signalArguments[0][1],
                    "2026-08-17T13:00:00.000Z")
            compare(invitationResponseSpy.signalArguments[0][2], 7)
            compare(invitationResponseSpy.signalArguments[0][3], "accepted")
            compare(invitationResponseSpy.signalArguments[0][4], "occurrence")
            invitationResponseSpy.target = null
            invitationSeenSpy.target = null
            activity.close()
            tryCompare(activity, "opened", false)
            activity.destroy()
            wait(0)

            const settings = createTemporaryObject(settingsFactory, scene, {
                "accounts": [
                    {"id": "account-local", "provider": "local",
                     "displayName": "On this device", "authStatus": "connected"},
                    {"id": "account-google", "provider": "google",
                     "displayName": "Google", "authStatus": "reauthorization_required"}
                ],
                "calendars": [
                    {"id": "calendar-writable", "accountId": "account-local",
                     "name": "Personal", "color": "#7aa2f7", "enabled": true,
                     "readOnly": false, "position": 0},
                    {"id": "calendar-read-only", "accountId": "account-ics",
                     "name": "Subscribed", "color": "#9ece6a", "enabled": true,
                     "readOnly": true, "position": 1},
                    {"id": "calendar-google", "accountId": "account-google",
                     "name": "Team", "color": "#f7768e", "enabled": true,
                     "readOnly": false, "position": 2,
                     "capabilities": {"provider": "google",
                                      "canDeleteCalendar": true,
                                      "primary": false}}
                ],
                "calendarSets": [{
                    "id": "set-focus", "name": "Focus",
                    "calendarIds": ["calendar-writable", "calendar-read-only"],
                    "defaultCalendarId": "calendar-writable"
                }],
                "connected": false,
                "preferences": {
                    "timeFormat": "24h", "firstDayOfWeek": 1,
                    "displayTimeZone": "America/New_York", "defaultDuration": 60,
                    "workDayStart": 8, "workDayEnd": 18,
                    "defaultCalendarId": "calendar-writable"
                }
            })
            verify(settings !== null)
            settings.open()
            tryCompare(settings, "opened", true)
            verifyFiniteGeometry(settings.contentItem, "account-settings")
            const deleteLocal = findChild(scene.Window.window.contentItem,
                                          "deleteLocalCalendar-calendar-writable")
            verify(deleteLocal !== null,
                   "non-default local calendar exposes a delete action")
            localCalendarRemovalSpy.target = settings
            localCalendarRemovalSpy.clear()
            deleteLocal.clicked()
            const deleteConfirm = findChild(scene.Window.window.contentItem,
                                            "deleteLocalCalendarConfirm")
            verify(deleteConfirm !== null)
            tryCompare(deleteConfirm, "opened", true)
            deleteConfirm.accept()
            compare(localCalendarRemovalSpy.count, 1)
            compare(localCalendarRemovalSpy.signalArguments[0][0],
                    "calendar-writable")
            const defaultCalendarSelector = findChild(
                        scene.Window.window.contentItem,
                        "defaultCalendarSelector")
            verify(defaultCalendarSelector !== null,
                   "calendar settings expose one default selector")
            compare(defaultCalendarSelector.count, 2,
                    "read-only calendars are excluded from the default selector")
            compare(defaultCalendarSelector.currentValue, "calendar-writable")
            preferenceChangedSpy.target = settings
            preferenceChangedSpy.clear()
            defaultCalendarSelector.activated(1)
            compare(preferenceChangedSpy.count, 1)
            compare(preferenceChangedSpy.signalArguments[0][0],
                    "defaultCalendarId")
            compare(preferenceChangedSpy.signalArguments[0][1],
                    "calendar-google")
            preferenceChangedSpy.target = null

            const calendarColorPicker = findChild(
                        scene.Window.window.contentItem,
                        "calendarColorPicker-calendar-google")
            verify(calendarColorPicker !== null,
                   "calendar exposes a compact color selector")
            verify(!calendarColorPicker.paletteVisible,
                   "calendar colors remain hidden by default")
            calendarColorPicker.openPalette()
            tryCompare(calendarColorPicker, "paletteVisible", true)
            calendarColorPicker.closePalette()
            tryCompare(calendarColorPicker, "paletteVisible", false)

            const compactCalendarCard = findChild(
                        scene.Window.window.contentItem,
                        "calendarCard-calendar-google")
            verify(compactCalendarCard !== null)
            verify(compactCalendarCard.implicitHeight <= 112,
                   "calendar detail card remains compact")

            const calendarDragHandle = findChild(
                        scene.Window.window.contentItem,
                        "calendarDragHandle-calendar-google")
            verify(calendarDragHandle !== null,
                   "calendar exposes a drag handle instead of an order number")
            const calendarDragPreview = findChild(
                        scene.Window.window.contentItem,
                        "calendarDragPreview-calendar-google")
            const calendarDropIndicator = findChild(
                        scene.Window.window.contentItem,
                        "calendarDropIndicator-calendar-writable")
            verify(calendarDragPreview !== null,
                   "dragging has a labeled floating preview")
            verify(calendarDropIndicator !== null,
                   "drop targets have a labeled insertion indicator")
            verify(!calendarDragPreview.visible)
            verify(!calendarDropIndicator.visible)
            const muteInvitationAlerts = findChild(
                        scene.Window.window.contentItem,
                        "muteInvitationAlerts-calendar-google")
            verify(muteInvitationAlerts !== null)
            compare(muteInvitationAlerts.text, "Mute invitation alerts")
            calendarPreferenceSpy.target = settings
            calendarPreferenceSpy.clear()
            settings.reorderCalendar("calendar-google",
                                     "calendar-writable", false)
            compare(calendarPreferenceSpy.count, 3)
            compare(calendarPreferenceSpy.signalArguments[0][0],
                    "calendar-google")
            compare(calendarPreferenceSpy.signalArguments[0][1], "position")
            compare(calendarPreferenceSpy.signalArguments[0][2], 0)
            calendarPreferenceSpy.target = null

            const deleteGoogle = findChild(scene.Window.window.contentItem,
                                           "deleteLocalCalendar-calendar-google")
            verify(deleteGoogle !== null,
                   "owned secondary Google calendar exposes a delete action")
            localCalendarRemovalSpy.clear()
            deleteGoogle.clicked()
            tryCompare(deleteConfirm, "opened", true)
            deleteConfirm.accept()
            compare(localCalendarRemovalSpy.count, 1)
            compare(localCalendarRemovalSpy.signalArguments[0][0],
                    "calendar-google")
            localCalendarRemovalSpy.target = null
            keyClick(Qt.Key_Tab)
            tryVerify(function() {
                return scene.Window.window.activeFocusItem !== null
            }, 2000, "Tab establishes a valid settings focus target")
            settings.close()
            tryCompare(settings, "opened", false)
            settings.destroy()
            wait(0)
        }

        function test_editor_preserves_absolute_and_provider_reminders() {
            const event = Object.assign({}, representativeEvents()[0])
            event.attendees = []
            const absoluteReminder = {
                "method": "popup",
                "at": "2026-08-17T11:45:00.000Z",
                "providerDefault": true,
                "xProvider": {"opaque": "keep"},
                "futureField": ["alpha", 7]
            }
            const relativeReminder = {
                "method": "email",
                "minutesBefore": 30,
                "xProvider": "keep-relative"
            }
            event.reminders = [absoluteReminder, relativeReminder]

            const editor = createTemporaryObject(editorFactory, scene)
            verify(editor !== null)
            editorSaveSpy.target = editor
            editorSaveSpy.clear()
            editor.openExisting(event)
            tryCompare(editor, "opened", true)

            const absoluteLabel = findChild(scene.Window.window.contentItem,
                                            "reminderLabel-0")
            verify(absoluteLabel !== null)
            verify(absoluteLabel.text.indexOf("Popup at") === 0)
            const absoluteEditor = findChild(scene.Window.window.contentItem,
                                             "relativeReminderEditor-0")
            verify(absoluteEditor !== null)
            verify(!absoluteEditor.visible,
                   "absolute reminder time is intentionally read-only")

            editor.submit()
            compare(editorSaveSpy.count, 1)
            const saved = editorSaveSpy.signalArguments[0][0]
            compare(saved.reminders.length, 2)
            compare(JSON.stringify(saved.reminders[0]),
                    JSON.stringify(absoluteReminder))
            compare(JSON.stringify(saved.reminders[1]),
                    JSON.stringify(relativeReminder))
            tryCompare(editor, "opened", false)

            editorSaveSpy.clear()
            editor.openExisting(event)
            tryCompare(editor, "opened", true)
            const relativeEditor = findChild(scene.Window.window.contentItem,
                                             "relativeReminderEditor-1")
            verify(relativeEditor !== null)
            verify(relativeEditor.visible)
            relativeEditor.activated(3)
            editor.submit()
            compare(editorSaveSpy.count, 1)
            const edited = editorSaveSpy.signalArguments[0][0]
            compare(JSON.stringify(edited.reminders[0]),
                    JSON.stringify(absoluteReminder))
            compare(edited.reminders[1].method, "email")
            compare(edited.reminders[1].minutes, 15)
            compare(edited.reminders[1].xProvider, "keep-relative")
            verify(edited.reminders[1].minutesBefore === undefined)
            tryCompare(editor, "opened", false)

            editorSaveSpy.clear()
            editor.openExisting(event)
            tryCompare(editor, "opened", true)
            const removeAbsolute = findChild(scene.Window.window.contentItem,
                                             "removeReminder-0")
            verify(removeAbsolute !== null)
            removeAbsolute.clicked()
            editor.submit()
            compare(editorSaveSpy.count, 1)
            const removed = editorSaveSpy.signalArguments[0][0]
            compare(removed.reminders.length, 1)
            compare(JSON.stringify(removed.reminders[0]),
                    JSON.stringify(relativeReminder))
            tryCompare(editor, "opened", false)

            editorSaveSpy.target = null
            editor.destroy()
            wait(0)
        }

        function test_fast_mutation_requires_explicit_scope_and_guest_policy() {
            const event = Object.assign({}, representativeEvents()[0], {
                "recurrenceRule": "FREQ=WEEKLY",
                "recurrenceId": "2026-08-17T13:00:00.000Z",
                "attendees": [
                    {"email": "me@example.com", "self": true},
                    {"email": "guest@example.com", "self": false}
                ]
            })
            const dialog = createTemporaryObject(mutationConfirmationFactory, scene)
            verify(dialog !== null)
            verify(dialog.needsChoiceFor(event))
            mutationConfirmedSpy.target = dialog
            mutationConfirmedSpy.clear()
            dialog.openFor(event, "Apply change", {"kind": "save"},
                           {"expectedLocalRevision": 7}, true)
            tryCompare(dialog, "opened", true)

            const recurrenceScope = findChild(scene.Window.window.contentItem,
                                               "mutationRecurrenceScope")
            const guestPolicy = findChild(scene.Window.window.contentItem,
                                          "mutationGuestPolicy")
            const confirmButton = findChild(scene.Window.window.contentItem,
                                            "confirmMutationButton")
            verify(recurrenceScope !== null)
            verify(guestPolicy !== null)
            verify(confirmButton !== null)
            verify(!confirmButton.enabled)

            recurrenceScope.currentIndex = 1
            recurrenceScope.activated(1)
            wait(0)
            verify(!confirmButton.enabled,
                   "guest policy remains an explicit independent choice")
            guestPolicy.currentIndex = 1
            guestPolicy.activated(1)
            wait(0)
            verify(confirmButton.enabled)
            confirmButton.clicked()
            compare(mutationConfirmedSpy.count, 1)
            const options = mutationConfirmedSpy.signalArguments[0][1]
            compare(options.recurrenceScope, "occurrence")
            compare(options.guestNotificationPolicy, "none")
            compare(options.expectedLocalRevision, 7)
            tryCompare(dialog, "opened", false)

            mutationConfirmedSpy.target = null
            dialog.destroy()
            wait(0)
        }

        function test_device_scale_is_valid() {
            verify(scene.Window.window !== null)
            verify(scene.Screen.devicePixelRatio >= 1.0)
            verify(isFinite(scene.Screen.devicePixelRatio))
        }
    }
}
