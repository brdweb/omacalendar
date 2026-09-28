import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtTest
import OmaCalendar
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
    Component { id: attendeeEditorFactory; Components.AttendeeEditor {} }
    Component { id: mutationConfirmationFactory; Components.MutationConfirmationDialog {} }
    Component { id: activityFactory; Components.ActivityPanel {} }
    Component { id: sidebarFactory; Components.CalendarSidebar {} }
    Component { id: quickAddFactory; Components.QuickAddDialog {} }
    Component { id: undoToastFactory; Components.UndoToast {} }
    Component { id: eventRowFactory; Components.EventRow {} }
    Component { id: availabilityFactory; Components.AvailabilityStrip {} }
    Component { id: conflictMergeFactory; Components.ConflictMergeDialog {} }
    Component { id: icsImportFactory; Components.IcsImportDialog {} }
    Component { id: icsExportFactory; Components.IcsExportDialog {} }
    Component { id: pdfExportFactory; Components.PdfExportDialog {} }
    Component { id: tasksPanelFactory; Components.TasksPanel {} }
    Component { id: taskEditorFactory; Components.TaskEditor {} }
    // Dialogs hand focus back through the window's content item, so the
    // focus test runs in an ApplicationWindow like the app's.
    Component {
        id: focusWindowFactory
        ApplicationWindow {
            property alias anchor: anchorInput
            width: 900
            height: 700
            visible: true
            TextInput { id: anchorInput; width: 100; height: 20 }
        }
    }
    Component { id: settingsFactory; Components.AccountSettingsDrawer {} }

    Component {
        id: signalSpyFactory
        SignalSpy {}
    }

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
                    else {
                        // YearView counts every day an event covers, like the
                        // agenda and month views, including the continuation
                        // of the overnight conference.
                        compare(populatedView.eventCount(scene.referenceDate), 6)
                        compare(populatedView.eventCount(
                                    new Date(2026, 7, 16, 12, 0, 0)), 1)
                        compare(populatedView.eventCount(
                                    new Date(2026, 7, 18, 12, 0, 0)), 1)
                        compare(populatedView.eventCount(
                                    new Date(2026, 7, 19, 12, 0, 0)), 0)
                    }
                    populatedView.destroy()
                    wait(0)
                }
            }
        }

        function test_agenda_grows_and_timelines_restore_scroll() {
            const profile = viewportProfiles()[0]
            const agenda = createView(agendaFactory, profile, representativeEvents())
            const list = findChild(agenda, "agendaList")
            verify(list !== null, "agenda list exists")
            compare(list.count, 31)
            const moreSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": agenda, "signalName": "moreDaysRequested"})
            list.positionViewAtEnd()
            tryVerify(function() { return moreSpy.count > 0 },
                      2000, "scrolling to the end asks for more days")

            const day = createTemporaryObject(dayFactory, scene, {
                "width": profile.width, "height": profile.height,
                "currentDate": scene.referenceDate, "events": [],
                "savedScrollY": 420, "visible": true})
            wait(0)
            compare(findChild(day, "dayTimelineFlick").contentY, 420)
            const week = createTemporaryObject(weekFactory, scene, {
                "width": profile.width, "height": profile.height,
                "currentDate": scene.referenceDate, "events": [],
                "savedScrollY": 300, "visible": true})
            wait(0)
            compare(findChild(week, "weekTimelineScroll").contentY, 300)
            // Without a saved position, a day that is not today opens an hour
            // before the work day.
            const fresh = createView(dayFactory, profile, [])
            compare(findChild(fresh, "dayTimelineFlick").contentY,
                    (fresh.workDayStart - 1) * fresh.pixelsPerHour)
        }

        function test_week_numbers_and_year_heat_map() {
            const month = createTemporaryObject(monthFactory, scene, {
                "width": 900, "height": 700, "currentDate": new Date(2026, 8, 15),
                "firstDayOfWeek": 1, "events": [], "visible": true})
            wait(0)
            const firstWeek = findChild(month, "monthWeekNumber-0")
            verify(firstWeek !== null)
            verify(!firstWeek.visible, "week numbers are off by default")
            month.showWeekNumbers = true
            tryCompare(firstWeek, "visible", true)
            compare(firstWeek.text, "36", "the grid opens on Monday August 31")
            compare(findChild(month, "monthWeekNumber-5").text, "41")

            const week = createTemporaryObject(weekFactory, scene, {
                "width": 900, "height": 700, "currentDate": new Date(2026, 8, 30),
                "firstDayOfWeek": 1, "showWeekNumbers": true, "events": [],
                "visible": true})
            wait(0)
            compare(findChild(week, "weekNumberLabel").text, "W40")

            const year = createTemporaryObject(yearFactory, scene, {
                "width": 1100, "height": 800, "currentDate": scene.referenceDate,
                "events": representativeEvents(), "visible": true})
            wait(0)
            verify(findChild(year, "yearHeatLegend") !== null)
            compare([0, 1, 2, 3, 4, 5, 6, 40].map(year.heatLevel),
                    [0, 1, 2, 2, 3, 3, 4, 4])
        }

        function test_drag_between_all_day_lane_and_timeline() {
            const allDay = representativeEvents().filter(function(value) {
                return value.allDay })[0]
            const timed = representativeEvents()[0]
            const week = createTemporaryObject(weekFactory, scene, {
                "width": 1000, "height": 700, "currentDate": scene.referenceDate,
                "events": [timed, allDay], "visible": true})
            wait(0)
            const timeSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": week, "signalName": "eventTimeChanged"})
            let accepted = false
            week.dropOnTimeline({"source": {"eventData": allDay},
                                 "acceptProposedAction": function() { accepted = true }},
                                week.weekStart, 600)
            compare(timeSpy.count, 1)
            verify(accepted)
            compare(timeSpy.signalArguments[0][2], 600)
            compare(timeSpy.signalArguments[0][3], week.defaultDurationMinutes,
                    "an all-day event takes the default length")

            const timelineEvent = createTemporaryObject(timelineEventFactory, scene, {
                "eventData": timed, "width": 120, "height": 60, "y": 500,
                "startMinute": 540, "durationMinutes": 60, "allDayDropY": 300})
            const allDaySpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": timelineEvent, "signalName": "allDayRequested"})
            const rescheduleSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": timelineEvent, "signalName": "rescheduleRequested"})
            timelineEvent.pressY = 10
            timelineEvent.commitMove(0, -150)
            compare(allDaySpy.count, 0, "a move that stays in the timeline")
            compare(rescheduleSpy.count, 1)
            timelineEvent.commitMove(0, -260)
            compare(allDaySpy.count, 1, "released above the timeline top")
            compare(rescheduleSpy.count, 1)
        }

        function test_sidebar_shows_account_sync_state() {
            const today = new Date()
            const sidebar = createTemporaryObject(sidebarFactory, scene, {
                "width": 280, "height": 900,
                "accounts": [{"id": "g", "provider": "google", "displayName": "Work"},
                             {"id": "l", "provider": "local", "displayName": "This device"},
                             {"id": "i", "provider": "ics", "displayName": "Holidays"}],
                "accountSyncStates": {
                    "g": {"state": "reauthorization_required", "message": "Token revoked"},
                    "i": {"state": "idle", "lastSyncAt": today.toISOString()}}})
            verify(sidebar !== null)
            wait(0)
            compare(findChild(sidebar, "accountSyncText-0").text, "Sign-in expired")
            const signIn = findChild(sidebar, "accountSyncAction-0")
            verify(signIn.visible)
            const reauthorize = createTemporaryObject(signalSpyFactory, testCase, {
                "target": sidebar, "signalName": "accountReauthorizeRequested"})
            signIn.clicked()
            compare(reauthorize.count, 1)
            compare(reauthorize.signalArguments[0][0], "g")
            verify(findChild(sidebar, "accountSyncText-1").text.indexOf("Synced ") === 0,
                   "the local account is not listed, so the subscription is second")
            verify(!findChild(sidebar, "accountSyncAction-1").visible)
            verify(findChild(sidebar, "accountSync-2") === null)

            sidebar.accountSyncStates = {"g": {"state": "error", "message": "Timeout"}}
            compare(findChild(sidebar, "accountSyncText-0").text, "Sync failed")
            const syncSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": sidebar, "signalName": "accountSyncRequested"})
            findChild(sidebar, "accountSyncAction-0").clicked()
            compare(syncSpy.count, 1)

            const attention = findChild(sidebar, "sidebarActivity")
            verify(!attention.visible, "nothing needs attention")
            sidebar.failedOperationCount = 2
            verify(attention.visible)
            const panelSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": sidebar, "signalName": "panelRequested"})
            attention.clicked()
            compare(panelSpy.signalArguments[0][0], "sync")
        }

        function test_activity_panel_reaches_conflicts_and_sync() {
            const panel = createTemporaryObject(activityFactory, scene)
            verify(panel !== null)
            verify(findChild(panel, "conflictsTab") !== null)
            verify(findChild(panel, "syncTab") !== null)
            panel.mode = "conflicts"
            compare(panel.mode, "conflicts", "a conflicts request is kept, not reset to search")
            panel.mode = "sync"
            compare(panel.mode, "sync")
        }

        function test_theme_text_meets_contrast() {
            const pairs = [[Theme.text, Theme.background], [Theme.text, Theme.surface],
                           [Theme.text, Theme.surfaceAlt], [Theme.text, Theme.darkBackground],
                           [Theme.mutedText, Theme.background],
                           [Theme.mutedText, Theme.surface],
                           [Theme.accentText, Theme.accent]]
            for (let index = 0; index < pairs.length; ++index) {
                const ratio = Theme.contrastRatio(pairs[index][0], pairs[index][1])
                verify(ratio >= 4.5, "token pair " + index + " reads at " + ratio.toFixed(2))
            }

            // Whatever the calendar colour, tint and theme, event text reads.
            const calendarColors = ["#ffffff", "#ffff00", "#00ff00", "#000000", "#1a1b26",
                                    "#7aa2f7", "#f7768e", "#808080"]
            const backgrounds = [Theme.background, "#fafafa", "#000000", "#ffffff"]
            const opacities = [0.13, 0.22, 0.28, 0.34, 0.9]
            for (let c = 0; c < calendarColors.length; ++c) {
                for (let b = 0; b < backgrounds.length; ++b) {
                    for (let o = 0; o < opacities.length; ++o) {
                        const fill = Theme.blend(Qt.color(calendarColors[c]), opacities[o],
                                                 Qt.color(backgrounds[b]))
                        for (const preferred of [Theme.text, Theme.mutedText]) {
                            const ratio = Theme.contrastRatio(
                                        Theme.readableText(fill, preferred), fill)
                            verify(ratio >= 4.5, calendarColors[c] + " at " + opacities[o]
                                   + " over " + backgrounds[b] + ": " + ratio.toFixed(2))
                        }
                    }
                }
            }
            compare(Theme.readableText(Theme.background, Theme.text), Theme.text,
                    "a readable preferred colour is kept")

            const chip = createTemporaryObject(eventChipFactory, scene, {
                "width": 200, "selected": true, "showTime": true, "timeText": "09:00",
                "eventData": Object.assign({}, representativeEvents()[0],
                                           {"calendarColor": "#ffffff"})})
            const summary = findChild(chip, "eventChipSummary")
            verify(Theme.contrastRatio(summary.color, chip.fillColor) >= 4.5)
            const block = createTemporaryObject(timelineEventFactory, scene, {
                "width": 160, "height": 60, "selected": true,
                "eventData": Object.assign({}, representativeEvents()[0],
                                           {"calendarColor": "#ffffff"})})
            verify(Theme.contrastRatio(findChild(block, "timelineEventSummary").color,
                                       block.fillColor) >= 4.5)
        }

        function test_dialogs_trap_and_restore_focus() {
            const openers = [
                [editorFactory, function(dialog) { dialog.openNew(new Date(2026, 8, 28), 540) }],
                [quickAddFactory, function(dialog) { dialog.openEmpty() }],
                [mutationConfirmationFactory, function(dialog) {
                    dialog.openFor(representativeEvents()[0], "Apply change",
                                   {"kind": "save"}, {}, true) }],
                [conflictMergeFactory, function(dialog) {
                    dialog.openFor({"id": "conflict", "localEvent": representativeEvents()[0],
                                    "remoteEvent": representativeEvents()[0]}) }],
                [icsImportFactory, function(dialog) { dialog.open() }],
                [icsExportFactory, function(dialog) { dialog.open() }]
            ]
            const focusWindow = createTemporaryObject(focusWindowFactory, testCase)
            tryCompare(focusWindow, "visible", true)
            focusWindow.requestActivate()
            const anchor = focusWindow.anchor
            for (let index = 0; index < openers.length; ++index) {
                anchor.forceActiveFocus()
                tryVerify(function() { return anchor.activeFocus }, 1000)
                const dialog = createTemporaryObject(openers[index][0],
                                                     focusWindow.contentItem)
                openers[index][1](dialog)
                tryCompare(dialog, "opened", true)
                verify(!anchor.activeFocus, "dialog " + index + " takes focus")
                verify(dialog.modal, "dialog " + index + " blocks the window behind it")
                dialog.close()
                tryCompare(dialog, "opened", false)
                tryVerify(function() { return anchor.activeFocus }, 1000,
                          "dialog " + index + " returns focus")
            }
        }

        function test_timelines_show_a_second_time_zone() {
            const hours = []
            for (let hour = 0; hour <= 24; ++hour)
                hours.push({"minute": ((hour * 60 + 570) % 1440),
                            "dayOffset": hour * 60 + 570 >= 1440 ? 1 : 0})
            const secondary = {"label": "Kolkata", "offsetLabel": "UTC+5:30", "hours": hours}
            const factories = [dayFactory, weekFactory]
            for (let index = 0; index < factories.length; ++index) {
                const view = createTemporaryObject(factories[index], scene, {
                    "width": 900, "height": 700, "currentDate": scene.referenceDate,
                    "events": [], "timeFormat": "24h", "visible": true})
                wait(0)
                const nine = findChild(view, "secondaryHourLabel-9")
                verify(nine !== null)
                verify(!nine.visible, "no second zone by default")
                verify(!findChild(view, "secondaryZoneCaption").parent.visible)
                view.secondaryTime = secondary
                compare(nine.text, "18:30")
                compare(findChild(view, "secondaryHourLabel-20").text, "05:30 +1")
                verify(findChild(view, "secondaryZoneCaption").parent.visible)
                compare(findChild(view, "secondaryZoneCaption").text, "Kolkata")
            }
        }

        function test_search_results_highlight_and_jump() {
            const event = Object.assign({}, representativeEvents()[0],
                                        {"summary": "<b>Design</b> review & Needle"})
            const row = createTemporaryObject(eventRowFactory, scene, {
                "eventData": event, "width": 400, "highlight": "needle design",
                "jumpOnClick": true})
            const title = findChild(row, "eventRowTitle")
            compare(title.textFormat, Text.StyledText)
            verify(title.text.indexOf("&lt;b&gt;") >= 0, "provider markup stays literal")
            verify(title.text.indexOf("&amp;") >= 0)
            verify(title.text.indexOf("<b>Design</b>") >= 0, "matches keep their case")
            verify(title.text.indexOf("<b>Needle</b>") >= 0)

            const jumpSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": row, "signalName": "jumpRequested"})
            const editSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": row, "signalName": "editRequested"})
            row.clicked()
            compare(jumpSpy.count, 0, "the jump waits to rule out a double-click")
            tryCompare(jumpSpy, "count", 1, 2000, "a single click jumps to the result")
            compare(editSpy.count, 0)
            // A double-click's first click must not jump.
            row.clicked()
            row.doubleClicked()
            compare(editSpy.count, 1, "a double-click edits it")
            wait(Qt.styleHints.mouseDoubleClickInterval + 100)
            compare(jumpSpy.count, 1, "and does not jump")

            row.highlight = ""
            compare(title.textFormat, Text.PlainText)
            compare(title.text, "<b>Design</b> review & Needle")
        }

        function test_search_filters_cover_time_and_guests() {
            const panel = createTemporaryObject(activityFactory, scene, {"connected": true})
            panel.open()
            tryCompare(panel, "opened", true)
            const requests = createTemporaryObject(signalSpyFactory, testCase, {
                "target": panel, "signalName": "searchRequested"})
            findChild(panel, "searchGuestFilter").text = "sam@example.com"
            const when = findChild(panel, "searchWhenFilter")
            when.currentIndex = 1
            panel.submitSearch()
            let filters = requests.signalArguments[requests.count - 1][1]
            compare(filters.attendee, "sam@example.com")
            verify(filters.start !== undefined && filters.end === undefined, "upcoming")
            when.currentIndex = 2
            panel.submitSearch()
            filters = requests.signalArguments[requests.count - 1][1]
            verify(filters.end !== undefined && filters.start === undefined, "past")
            verify(!findChild(panel, "offlineSearchNote").visible)
            panel.connected = false
            verify(findChild(panel, "offlineSearchNote").visible)
        }

        function freeBusyFixture() {
            return {"requestId": "request-1", "start": "2026-09-28T00:00:00.000Z",
                    "end": "2026-10-05T00:00:00.000Z",
                    "self": [{"start": "2026-09-28T09:00:00.000Z",
                              "end": "2026-09-28T10:00:00.000Z"}],
                    "attendees": {"sam@example.com": [{"start": "2026-09-28T13:00:00.000Z",
                                                       "end": "2026-09-28T14:00:00.000Z"}]},
                    "pending": ["pat@example.com"],
                    "unavailable": [{"email": "out@example.org", "reason": "notFound"}]}
        }

        function test_availability_strip_marks_conflicts() {
            const strip = createTemporaryObject(availabilityFactory, scene, {
                "width": 600, "freeBusy": freeBusyFixture(),
                "guests": ["Sam@example.com", "pat@example.com", "out@example.org"],
                "dateText": "2026-09-28", "startText": "09:30", "endText": "10:30",
                "timeZone": "UTC"})
            wait(0)
            compare(strip.rows.length, 4)
            compare(findChild(strip, "availabilityStatus-0").text, "Busy", "you, 09:00-10:00")
            compare(findChild(strip, "availabilityStatus-1").text, "Free", "guest matched case-insensitively")
            compare(findChild(strip, "availabilityStatus-2").text, "Checking…")
            compare(findChild(strip, "availabilityStatus-3").text, "Not shared")
            verify(strip.anyConflict)
            strip.startText = "13:30"
            strip.endText = "14:30"
            compare(findChild(strip, "availabilityStatus-0").text, "Free")
            compare(findChild(strip, "availabilityStatus-1").text, "Busy")
            verify(!findChild(strip, "findFreeTime").enabled,
                   "a pending guest's time is unknown, not free")
            const answered = freeBusyFixture()
            answered.pending = []
            answered.attendees["pat@example.com"] = []
            strip.freeBusy = answered
            verify(findChild(strip, "findFreeTime").enabled)
            const requested = createTemporaryObject(signalSpyFactory, testCase, {
                "target": strip, "signalName": "slotRequested"})
            findChild(strip, "findFreeTime").clicked()
            compare(requested.count, 1)
        }

        function test_editor_asks_for_availability_and_finds_a_time() {
            App.freeBusy = ({})
            App.lastFreeBusyQuery = null
            const editor = createTemporaryObject(editorFactory, scene)
            const event = Object.assign({}, representativeEvents()[0], {
                "startUtc": "2026-09-28T09:00:00.000Z", "endUtc": "2026-09-28T10:00:00.000Z",
                "displayStartLocal": "2026-09-28T09:00:00", "displayEndLocal": "2026-09-28T10:00:00",
                "startTimeZone": "UTC", "endTimeZone": "UTC",
                "attendees": [{"email": "sam@example.com"}]})
            editor.openExisting(event)
            tryCompare(editor, "opened", true)
            tryVerify(function() { return App.lastFreeBusyQuery !== null }, 2000,
                      "the editor asks who is busy")
            compare(App.lastFreeBusyQuery.emails, ["sam@example.com"])
            compare(App.lastFreeBusyQuery.excludeEventId, event.id,
                    "the event being edited does not count against you")
            verify(!editor.showAvailability, "nothing to show before the answer")

            const answer = freeBusyFixture()
            answer.start = App.lastFreeBusyQuery.start
            App.freeBusy = answer
            tryCompare(editor, "showAvailability", true)
            App.nextFreeSlotResult = "2026-09-28T11:00:00.000Z"
            // A guest is still pending, so the editor will not guess.
            editor.findFreeTime()
            compare(findChild(scene.Window.window.contentItem, "eventStartTime").text, "09:00")
            answer.pending = []
            App.freeBusy = Object.assign({}, answer)
            findChild(scene.Window.window.contentItem, "findFreeTime").clicked()
            compare(findChild(scene.Window.window.contentItem, "freeTimeMessage").text,
                    "Moved to the next time everyone checked is free.")
            const content = scene.Window.window.contentItem
            compare(findChild(content, "eventStartDate").text, "2026-09-28")
            compare(findChild(content, "eventStartTime").text, "11:00")
            compare(findChild(content, "eventEndTime").text, "12:00", "the length is kept")
            App.freeBusy = ({})
        }

        function test_editor_offers_meet_and_lists_attachments() {
            const editor = createTemporaryObject(editorFactory, scene)
            const saveSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": editor, "signalName": "saveRequested"})
            const content = scene.Window.window.contentItem
            editor.openExisting(Object.assign({}, representativeEvents()[0],
                                              {"calendarId": "calendar-google",
                                               "attendees": []}))
            tryCompare(editor, "opened", true)
            compare(App.lastAttachmentLookup.eventId, "event-timed",
                    "opening an event asks for its attachments")
            const meet = findChild(content, "addMeetCheckBox")
            verify(meet.visible, "a Google calendar that accepts Meet offers it")
            App.eventAttachments = {"eventId": "event-timed", "recurrenceId": "",
                "attachments": [{"title": "Agenda.pdf",
                                 "url": "https://files.example.com/agenda.pdf"}]}
            compare(editor.attachments.length, 1)
            meet.checked = true
            editor.submit()
            compare(editor.validationError, "")
            compare(saveSpy.count, 1)
            compare(saveSpy.signalArguments[0][0].addConference, true)

            editor.openExisting(Object.assign({}, representativeEvents()[0], {
                "calendarId": "calendar-google",
                "conferenceUrl": "https://meet.google.com/abc-defg-hij"}))
            tryCompare(editor, "opened", true)
            verify(!meet.visible, "an event with a conference is not offered another")
            verify(findChild(content, "joinConferenceButton").visible)

            editor.openExisting(Object.assign({}, representativeEvents()[0],
                                              {"attendees": []}))
            tryCompare(editor, "opened", true)
            verify(!meet.visible, "a local calendar cannot create Meet links")
            compare(editor.attachments.length, 0,
                    "an answer for a different lookup is not shown")
            editor.submit()
            verify(!("addConference" in saveSpy.signalArguments[1][0]))
            editor.close()
        }

        function test_tasks_panel_groups_adds_and_completes() {
            const today = new Date()
            const key = function(offset) {
                return Qt.formatDate(new Date(today.getFullYear(), today.getMonth(),
                                              today.getDate() + offset), "yyyy-MM-dd")
            }
            const panel = createTemporaryObject(tasksPanelFactory, scene, {
                "taskLists": [{"id": "local-tasks", "name": "Tasks", "color": "#9ece6a"},
                              {"id": "work", "name": "Work", "color": "#7aa2f7",
                               "readOnly": true}],
                "tasks": [
                    {"id": "late", "listId": "local-tasks", "title": "Pay rent",
                     "dueDate": key(-1)},
                    {"id": "now", "listId": "work", "title": "Send report",
                     "dueDate": key(0)},
                    {"id": "done", "listId": "local-tasks", "title": "Old",
                     "completed": true}]})
            const created = createTemporaryObject(signalSpyFactory, testCase, {
                "target": panel, "signalName": "createRequested"})
            const completed = createTemporaryObject(signalSpyFactory, testCase, {
                "target": panel, "signalName": "completionRequested"})
            panel.open()
            tryCompare(panel, "opened", true)
            compare(panel.rows.map(function(row) {
                return row.kind === "header" ? row.key : row.task.id
            }), ["overdue", "late", "today", "now"], "completed tasks stay hidden")

            const content = scene.Window.window.contentItem
            const field = findChild(content, "addTaskField")
            field.text = "Buy milk"
            field.accepted()
            compare(created.count, 1)
            compare(created.signalArguments[0][0].title, "Buy milk")
            compare(created.signalArguments[0][0].listId, "local-tasks",
                    "new tasks go into a writable list")
            compare(field.text, "")

            const box = findChild(content, "taskDone-late")
            verify(box !== null)
            box.toggle()
            box.toggled()
            compare(completed.count, 1)
            compare(completed.signalArguments[0][0], "late")
            compare(completed.signalArguments[0][1], true)

            panel.showCompleted = true
            compare(panel.rows[panel.rows.length - 1].task.id, "done")
            panel.close()
        }

        function test_agenda_lists_tasks_due_that_day() {
            const agenda = createTemporaryObject(agendaFactory, scene, {
                "width": 600, "height": 500, "currentDate": new Date(2026, 9, 5),
                "dayCount": 3, "events": [],
                "taskLists": [{"id": "local-tasks", "name": "Tasks"}],
                "tasks": [{"id": "due", "listId": "local-tasks", "title": "File taxes",
                           "dueDate": "2026-10-06"},
                          {"id": "done", "listId": "local-tasks", "title": "Old",
                           "dueDate": "2026-10-06", "completed": true}]})
            const completion = createTemporaryObject(signalSpyFactory, testCase, {
                "target": agenda, "signalName": "taskCompletionRequested"})
            const activated = createTemporaryObject(signalSpyFactory, testCase, {
                "target": agenda, "signalName": "taskActivated"})
            const row = findChild(agenda, "agendaTask-due")
            verify(row !== null, "an open task shows on its due day")
            verify(findChild(agenda, "agendaTask-done") === null,
                   "completed tasks are not listed")
            row.clicked()
            compare(activated.count, 1)
            row.contentItem.children[0].toggle()
            row.contentItem.children[0].toggled()
            compare(completion.count, 1)
            compare(completion.signalArguments[0][0], "due")
        }

        function test_task_editor_validates_and_saves() {
            const editor = createTemporaryObject(taskEditorFactory, scene, {
                "taskLists": [{"id": "local-tasks", "name": "Tasks"},
                              {"id": "shared", "name": "Shared", "readOnly": true}]})
            const saved = createTemporaryObject(signalSpyFactory, testCase, {
                "target": editor, "signalName": "saveRequested"})
            const removed = createTemporaryObject(signalSpyFactory, testCase, {
                "target": editor, "signalName": "removeRequested"})
            const content = scene.Window.window.contentItem
            editor.openNew("local-tasks", "")
            tryCompare(editor, "opened", true)
            findChild(content, "taskTitle").text = "Renew passport"
            findChild(content, "taskDue").text = "2026-02-30"
            editor.submit()
            compare(saved.count, 0, "an impossible date is refused")
            verify(editor.validationError.length > 0)
            findChild(content, "taskDue").text = "2026-10-15"
            editor.submit()
            compare(saved.count, 1)
            compare(saved.signalArguments[0][0].listId, "local-tasks")
            compare(saved.signalArguments[0][0].dueDate, "2026-10-15")

            editor.openExisting({"id": "task-9", "listId": "local-tasks",
                                 "title": "Call Sam", "localRevision": 3})
            tryCompare(editor, "opened", true)
            findChild(content, "taskDone").checked = true
            editor.submit()
            compare(saved.count, 2)
            const update = saved.signalArguments[1][0]
            compare(update.id, "task-9")
            compare(update.completed, true)
            compare(update.localRevision, 3)
            verify(!("listId" in update), "edits never move a task")

            editor.openExisting({"id": "task-9", "listId": "local-tasks", "title": "x"})
            tryCompare(editor, "opened", true)
            const remove = findChild(content, "taskDelete")
            remove.clicked()
            compare(removed.count, 0, "deleting needs a second click")
            remove.clicked()
            compare(removed.count, 1)

            editor.openExisting({"id": "task-2", "listId": "shared", "title": "Theirs"})
            tryCompare(editor, "opened", true)
            verify(editor.readOnly)
            verify(!findChild(content, "taskSave").enabled)
            editor.close()
        }

        function test_pdf_dialog_checks_the_range() {
            const dialog = createTemporaryObject(pdfExportFactory, scene)
            const chosen = createTemporaryObject(signalSpyFactory, testCase, {
                "target": dialog, "signalName": "optionsChosen"})
            dialog.openFor({"first": new Date(2026, 8, 1), "last": new Date(2026, 8, 30)},
                           "month")
            tryCompare(dialog, "opened", true)
            const content = scene.Window.window.contentItem
            compare(findChild(content, "pdfFirstDate").text, "2026-09-01")
            compare(findChild(content, "pdfLayout").currentIndex, 1)
            verify(!findChild(content, "pdfIncludeDetails").enabled,
                   "a month grid has no room for details")
            findChild(content, "pdfChooseDestination").clicked()
            compare(chosen.count, 1)
            const options = chosen.signalArguments[0][0]
            compare(options.layout, "month")
            compare(options.lastDate, "2026-09-30")
            compare(options.visibleOnly, true)

            findChild(content, "pdfLastDate").text = "2026-08-01"
            findChild(content, "pdfChooseDestination").clicked()
            compare(chosen.count, 1, "a backwards range is refused")
            verify(dialog.validationError.length > 0)
            findChild(content, "pdfLastDate").text = "2027-12-31"
            verify(dialog.chosenOptions() === null, "more than a year is refused")
            dialog.close()
        }

        function test_undo_toast_offers_undo_only_when_possible() {
            const toast = createTemporaryObject(undoToastFactory, scene)
            verify(!toast.visible)
            const undoSpy = createTemporaryObject(signalSpyFactory, testCase, {
                "target": toast, "signalName": "undoRequested"})
            toast.show("Event deleted", true)
            verify(toast.visible)
            compare(findChild(toast, "undoToastMessage").text, "Event deleted")
            const action = findChild(toast, "undoToastAction")
            verify(action.visible)
            action.clicked()
            compare(undoSpy.count, 1)
            verify(!toast.shown, "undoing dismisses the toast")
            toast.show("Event moved", false)
            verify(!action.visible, "a change that cannot be undone offers no Undo")
        }

        function test_editor_keeps_recurrence_rules() {
            const rules = ["FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;COUNT=5",
                           "FREQ=MONTHLY;BYDAY=3MO",
                           "FREQ=MONTHLY;BYSETPOS=-1;BYDAY=MO,TU,WE,TH,FR"]
            const editor = createTemporaryObject(editorFactory, scene)
            verify(editor !== null)
            for (let index = 0; index < rules.length; ++index) {
                const event = Object.assign({}, representativeEvents()[0],
                                            {"recurrenceRule": rules[index]})
                editor.openExisting(event)
                tryCompare(editor, "opened", true)
                compare(editor.recurrenceRule(), rules[index],
                        "an unedited rule is saved unchanged")
                editor.close()
                tryCompare(editor, "opened", false)
            }

            const weekly = Object.assign({}, representativeEvents()[0],
                                         {"recurrenceRule": rules[0]})
            editor.openExisting(weekly)
            tryCompare(editor, "opened", true)
            const friday = findChild(scene.Window.window.contentItem,
                                     "recurrenceWeekday-FR")
            verify(friday !== null)
            friday.clicked()
            compare(editor.recurrenceRule(),
                    "FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE,FR;COUNT=5")
            const preview = findChild(scene.Window.window.contentItem,
                                      "recurrencePreview")
            compare(preview.text,
                    "Every 2 weeks on Monday, Wednesday and Friday, 5 times")
            editor.close()
        }

        function test_quick_add_draft_prefills_the_editor() {
            const editor = createTemporaryObject(editorFactory, scene)
            verify(editor !== null)
            editorSaveSpy.target = editor
            editorSaveSpy.clear()

            editor.openDraft({"title": "Trip to Denver", "location": "", "recurrenceRule": "",
                              "date": "2026-10-03", "endDate": "2026-10-07",
                              "allDay": true, "startMinute": -1, "durationMinutes": 0},
                             new Date(2026, 8, 28))
            tryCompare(editor, "opened", true)
            editor.submit()
            compare(editorSaveSpy.count, 1)
            const trip = editorSaveSpy.signalArguments[0][0]
            compare(trip.summary, "Trip to Denver")
            compare(trip.allDay, true)
            compare(trip.startDate, "2026-10-03")
            compare(trip.endDate, "2026-10-07", "the exclusive end survives the editor")
            tryCompare(editor, "opened", false)

            editorSaveSpy.clear()
            editor.openDraft({"title": "Standup", "location": "Room 4",
                              "recurrenceRule": "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR",
                              "date": "", "endDate": "", "allDay": false,
                              "startMinute": 555, "durationMinutes": 15},
                             new Date(2026, 8, 28))
            tryCompare(editor, "opened", true)
            editor.submit()
            compare(editorSaveSpy.count, 1)
            const standup = editorSaveSpy.signalArguments[0][0]
            compare(standup.summary, "Standup")
            compare(standup.location, "Room 4")
            compare(standup.allDay, false)
            compare(standup.recurrenceRule, "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR")
            compare((Date.parse(standup.endUtc) - Date.parse(standup.startUtc)) / 60000, 15)
            tryCompare(editor, "opened", false)
        }

        function test_attendee_chips_keep_guests_and_flag_bad_addresses() {
            const guests = createTemporaryObject(attendeeEditorFactory, scene, {
                "width": 500, "organizerEmail": "avery@example.com"})
            verify(guests !== null)
            guests.load([{"email": "avery@example.com", "displayName": "Avery"},
                         {"email": "me@example.com", "partstat": "ACCEPTED",
                          "xProvider": "keep"}])
            wait(0)
            const organizerChip = findChild(guests, "attendeeChip-0")
            verify(organizerChip !== null)
            verify(organizerChip.Accessible.name.indexOf("organizer") >= 0)
            verify(findChild(guests, "attendeeChip-1").Accessible.name.indexOf("Accepted") >= 0)

            const input = findChild(guests, "attendeeInput")
            input.text = "bob@example.com, ME@example.com"
            verify(guests.commitInput())
            compare(guests.attendees.length, 3, "duplicates are ignored case-insensitively")
            compare(input.text, "")

            input.text = "not-an-address"
            verify(!guests.commitInput())
            compare(input.text, "not-an-address", "a bad address stays for correction")
            verify(guests.validationError().indexOf("not-an-address") >= 0)
            input.text = ""
            guests.invalidEntry = ""

            guests.removeAt(2)
            const saved = guests.result()
            compare(saved.length, 2)
            compare(saved[1].xProvider, "keep", "provider attendee fields survive")
            compare(saved[1].partstat, "ACCEPTED")
        }

        function test_editor_blocks_a_pending_invalid_guest() {
            const editor = createTemporaryObject(editorFactory, scene)
            editorSaveSpy.target = editor
            editorSaveSpy.clear()
            editor.openDraft({"title": "Planning", "startMinute": 540,
                              "durationMinutes": 30}, new Date(2026, 8, 28))
            tryCompare(editor, "opened", true)
            const input = findChild(scene.Window.window.contentItem, "attendeeInput")
            verify(input !== null)
            // Suggestions on screen keep editingFinished from committing.
            input.parent.suggestions = [{"email": "bogus@example.com"}]
            input.text = "bogus"
            editor.submit()
            compare(editorSaveSpy.count, 0, "a rejected guest address blocks saving")
            verify(editor.validationError.indexOf("bogus") >= 0)
            verify(editor.opened)
            editor.close()
        }

        function test_provider_markup_remains_literal() {
            const marker = "<b>literal & event text</b>"
            const location = "<img src='file:///etc/passwd'>"
            const event = Object.assign({}, representativeEvents()[0], {
                "summary": marker,
                "location": location
            })

            const chip = createTemporaryObject(eventChipFactory, scene, {
                "eventData": event,
                "width": 360,
                "height": 40,
                "compact": false
            })
            verify(chip !== null)
            const chipSummary = findChild(chip, "eventChipSummary")
            verify(chipSummary !== null)
            compare(chipSummary.text, marker)
            compare(chipSummary.textFormat, Text.PlainText)
            chip.destroy()
            wait(0)

            const timelineEvent = createTemporaryObject(timelineEventFactory, scene, {
                "eventData": event,
                "width": 320,
                "height": 64,
                "startMinute": 540,
                "durationMinutes": 60
            })
            verify(timelineEvent !== null)
            const timelineSummary = findChild(timelineEvent,
                                              "timelineEventSummary")
            const timelineLocation = findChild(timelineEvent,
                                               "timelineEventLocation")
            verify(timelineSummary !== null)
            verify(timelineLocation !== null)
            compare(timelineSummary.text, marker)
            compare(timelineSummary.textFormat, Text.PlainText)
            compare(timelineLocation.text, location)
            compare(timelineLocation.textFormat, Text.PlainText)
            timelineEvent.destroy()
            wait(0)

            const mutation = createTemporaryObject(mutationConfirmationFactory,
                                                   scene, {"eventData": event})
            verify(mutation !== null)
            const mutationSummary = findChild(mutation, "mutationEventSummary")
            verify(mutationSummary !== null)
            compare(mutationSummary.text, marker)
            compare(mutationSummary.textFormat, Text.PlainText)
            mutation.destroy()
            wait(0)
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

        function test_caldav_storage_proof_does_not_enable_future_rsvp() {
            const activity = createTemporaryObject(activityFactory, scene, {
                "mode": "invitations",
                "calendars": [{"id": "range-storage", "accountId": "caldav",
                    "readOnly": false, "capabilities": {"provider": "caldav",
                        "serverScheduling": true, "attendeeWrites": true,
                        "thisAndFuture": true, "rsvpThisAndFuture": false}}],
                "accounts": [{"id": "caldav", "provider": "caldav"}]
            })
            verify(activity !== null)
            activity.open()
            tryCompare(activity, "opened", true)
            invitationResponseSpy.target = activity
            invitationResponseSpy.clear()
            activity.requestInvitationResponse({"id": "scoped-invite",
                "calendarId": "range-storage", "recurrenceRule": "FREQ=DAILY;COUNT=3",
                "recurrenceId": "2030-03-10T13:00:00Z", "seen": true,
                "localRevision": 1}, "accepted")
            wait(0)
            const choices = activity.invitationScopeChoices()
            compare(choices.length, 3)
            verify(!choices.some(function(choice) { return choice.value === "future" }))
            const message = findChild(scene.Window.window.contentItem, "invitationFutureScopeMessage")
            verify(message !== null)
            tryCompare(message, "visible", true)
            activity.completeInvitationResponse("future")
            compare(invitationResponseSpy.count, 0)
            activity.completeInvitationResponse("occurrence")
            compare(invitationResponseSpy.count, 1)
            compare(invitationResponseSpy.signalArguments[0][4], "occurrence")
        }

        function test_future_support_requires_successful_check() {
            const editor = createTemporaryObject(editorFactory, scene)
            verify(editor !== null)
            for (const calendarId of ["calendar-unproven-caldav", "calendar-failed-caldav"]) {
                const event = Object.assign({}, representativeEvents()[0], {
                    "recurrenceRule": "FREQ=WEEKLY", "calendarId": calendarId
                })
                editor.openExisting(event)
                tryCompare(editor, "opened", true)
                verify(!editor.futureScopeSupported)
                verify(editor.futureScopeCheckAvailable)
                const check = findChild(scene.Window.window.contentItem, "checkFutureSupport")
                const scope = findChild(scene.Window.window.contentItem, "eventRecurrenceScope")
                const message = findChild(scene.Window.window.contentItem, "futureSupportMessage")
                verify(check !== null && check.visible && check.enabled)
                compare(scope.count, 3)
                App.lastProbeCalendarId = ""
                check.clicked()
                compare(App.lastProbeCalendarId, calendarId)
                compare(scope.count, 3, "requesting a check must not grant future scope")
                verify(message.text.indexOf(calendarId === "calendar-failed-caldav"
                                            ? "remain disabled" : "temporary test event") >= 0)
                editor.close()
                tryCompare(editor, "opened", false)
            }
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
                    {"id": "local-default", "accountId": "account-local",
                     "name": "Personal", "color": "#89b4fa", "enabled": true,
                     "readOnly": false, "position": 0,
                     "capabilities": {"provider": "local",
                                      "canDeleteCalendar": false}},
                    {"id": "calendar-writable", "accountId": "account-local",
                     "name": "Personal", "color": "#7aa2f7", "enabled": true,
                     "readOnly": false, "position": 1},
                    {"id": "calendar-read-only", "accountId": "account-ics",
                     "name": "Subscribed", "color": "#9ece6a", "enabled": true,
                     "readOnly": true, "position": 2},
                    {"id": "calendar-google", "accountId": "account-google",
                     "name": "Team", "color": "#f7768e", "enabled": true,
                     "readOnly": false, "position": 3,
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
                    "defaultCalendarId": "calendar-writable",
                    "notificationPrivacy": "generic"
                }
            })
            verify(settings !== null)
            settings.open()
            tryCompare(settings, "opened", true)
            verifyFiniteGeometry(settings.contentItem, "account-settings")
            const localDefaultProvider = findChild(
                        scene.Window.window.contentItem,
                        "calendarProvider-local-default")
            verify(localDefaultProvider !== null)
            compare(localDefaultProvider.text, "On this device · Built in")
            const deleteLocalDefault = findChild(
                        scene.Window.window.contentItem,
                        "deleteLocalCalendar-local-default")
            verify(deleteLocalDefault !== null)
            verify(!deleteLocalDefault.visible)
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
            compare(defaultCalendarSelector.count, 3,
                    "read-only calendars are excluded from the default selector")
            compare(defaultCalendarSelector.currentValue, "calendar-writable")
            preferenceChangedSpy.target = settings
            preferenceChangedSpy.clear()
            defaultCalendarSelector.activated(2)
            compare(preferenceChangedSpy.count, 1)
            compare(preferenceChangedSpy.signalArguments[0][0],
                    "defaultCalendarId")
            compare(preferenceChangedSpy.signalArguments[0][1],
                    "calendar-google")

            const notificationPrivacy = findChild(
                        scene.Window.window.contentItem,
                        "notificationPrivacy")
            verify(notificationPrivacy !== null,
                   "settings expose notification privacy")
            compare(notificationPrivacy.currentValue, "generic")
            notificationPrivacy.currentIndex = 2
            notificationPrivacy.activated(2)
            compare(preferenceChangedSpy.count, 2)
            compare(preferenceChangedSpy.signalArguments[1][0],
                    "notificationPrivacy")
            compare(preferenceChangedSpy.signalArguments[1][1],
                    "full_details")
            preferenceChangedSpy.target = null

            const calendarSetDialog = findChild(
                        scene.Window.window.contentItem, "calendarSetDialog")
            verify(calendarSetDialog !== null)
            calendarSetDialog.openExisting(settings.calendarSets[0])
            tryCompare(calendarSetDialog, "opened", true)
            const moveReadOnlyEarlier = findChild(
                        scene.Window.window.contentItem,
                        "calendarSetMoveEarlier-calendar-read-only")
            verify(moveReadOnlyEarlier !== null)
            verify(moveReadOnlyEarlier.enabled)
            moveReadOnlyEarlier.clicked()
            compare(calendarSetDialog.selectedIds[0], "calendar-read-only")
            compare(calendarSetDialog.selectedIds[1], "calendar-writable")
            compare(calendarSetDialog.orderedCalendars()[0].id,
                    "calendar-read-only")
            compare(calendarSetDialog.orderedCalendars()[1].id,
                    "calendar-writable")
            calendarSetDialog.close()
            tryCompare(calendarSetDialog, "opened", false)

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
            compare(muteInvitationAlerts.text, "Mute alerts")
            calendarPreferenceSpy.target = settings
            calendarPreferenceSpy.clear()
            settings.reorderCalendar("calendar-google",
                                     "calendar-writable", false)
            compare(calendarPreferenceSpy.count, 3)
            compare(calendarPreferenceSpy.signalArguments[0][0],
                    "calendar-google")
            compare(calendarPreferenceSpy.signalArguments[0][1], "position")
            compare(calendarPreferenceSpy.signalArguments[0][2], 1)
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
