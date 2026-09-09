# Candidate testing for 2026-09-09

Agent-executed results and reproduced candidate failures are recorded in
[the September 9 acceptance report](ACCEPTANCE_2026-09-09.md). Its scoped
passes supplement this checklist; the original RC3 candidate remains failed,
and unperformed portions of each row remain pending.

Test app `1.0.0-rc.4` and widget `0.1.0-rc.4`. These are candidate drafts;
the last public release remains beta until acceptance is complete. Use the
[installation guide](INSTALL.md) and the `SHA256SUMS` shipped with the draft.
Record the app/widget commit, package filename and hash, OS/desktop version,
provider/server version, and pass/fail/unavailable for each row. Do not include
credentials, private calendar names, personal events, or provider URLs in reports.

Use fresh RC4 downloads when its replacement draft is available. RC3 failed
expanded runtime acceptance; older receipts do not qualify the new artifacts.

## Before starting

1. Save a native-profile backup using [backup and recovery](BACKUP_AND_RECOVERY.md).
   Keep the published beta package for recovery. Restore the older backup before
   running an older binary; in-place database downgrade is not supported.
2. Choose disposable provider calendars and synthetic event names. Google tests
   need a primary calendar plus a disposable owned secondary calendar. Never
   delete a personal calendar to satisfy a test.
3. Install the native Arch candidate on Omarchy for app/widget tests. Test the
   `.deb` package on Ubuntu 26.04. Test Flatpak separately: it has its own profile,
   credentials, runtime, and daemon; the native widget does not use that profile.
4. Confirm the reported app version is `1.0.0-rc.4` and widget version is
   `0.1.0-rc.4`. Record the exact downloaded hashes before changing anything.

## Native installation and upgrade

| Check | Expected result | Result |
|---|---|---|
| Clean current-Omarchy VM | Fresh native install, socket activation, desktop/widget launch, upgrade and removal work in a complete desktop session | PENDING |
| Upgrade beta to candidate | Existing events, accounts, colors, visibility/order, default calendar, reminders and settings survive | PENDING |
| Schema 1 reconnect/reset | A disposable copy of a legacy profile is archived with private permissions, requests reconnection and completes provider sync as described in Backup and recovery | PENDING |
| Socket activation | With the desktop closed and daemon stopped, `omacalendarctl system.info` starts the backend through the enabled socket | PENDING |
| Desktop activation | Launcher, `.ics` opening and `omacalendar://` links focus one app window; repeated activation does not duplicate it | PENDING |
| Restart | App and daemon restarts preserve data and provider login; no repeated consent or duplicate remote writes | PENDING |
| Backup restore | Restore a synthetic backup and verify its events/settings; restore the older backup before testing the older package | PENDING |
| Removal/reinstall | Package-owned integration is removed, data is retained, reinstall restores access; only remove test profiles deliberately | PENDING |

## Desktop workflows

Repeat applicable event actions in Agenda, Day, Week, Month and Year. For Year,
verify selecting a day and opening/editing its event workflow.

| Check | Expected result | Result |
|---|---|---|
| Navigation/search | Today/date changes, keyboard navigation, search and selecting a result land on the correct event | PENDING |
| Calendar settings | Visibility, ordering, color and default calendar persist; deletion is offered only when permitted | PENDING |
| Timed/all-day/multi-day | Create, edit, move, delete and undo preserve dates and time zones across views | PENDING |
| Recurrence | Series and individual-occurrence edits/cancellation affect the intended instances; unsupported scopes are explained | PENDING |
| Drag/resize | Dense overlapping events remain responsive; time/day changes are correct and undo works | PENDING |
| Invitations | RSVP updates remotely where supported and the completed invitation leaves the pending list | PENDING |
| Offline/reconnect | Cached views and local edits work offline; reconnect drains once and exposes actionable failures | PENDING |
| Conflict | A competing remote/local edit exposes the conflict and each applicable resolution preserves the chosen data | PENDING |
| Reminders | Trigger, snooze/dismiss, restart and suspend/resume do not lose or duplicate reminders; privacy setting is respected | PENDING |
| Import/export | Synthetic ICS import, duplicate choices, export and re-import preserve recurrence/alarms/dates | PENDING |
| Accessibility | Keyboard focus is visible, dialogs trap/restore focus correctly, labels are legible, Escape and shortcuts work | PENDING |
| Displays | 100%, 125% and 200% scale, mixed-scale monitors and keyboard-driven movement show no clipped actions or unusable dialogs | PENDING |
| Repeated shutdown | Closing/reopening desktop and widget repeatedly causes no crash, stale lock, duplicate window or lost change | PENDING |

## Live providers

Use [Google testing](GOOGLE_TESTING.md) and [CalDAV testing](CALDAV_TESTING.md)
for the complete matrices. Mark an unsupported provider capability N/A with
server capability evidence, not PASS. An unavailable account remains pending.

| Target | Required evidence | Result |
|---|---|---|
| Google external account | No unverified-app bypass; discover primary/secondary calendars; writes/readback, all-day, recurrence/exception, guest policy/RSVP, offline/restart, token reuse and disconnect | PENDING |
| Google secondary deletion | Delete only a disposable owned secondary test calendar after confirmation; primary/non-owned calendars cannot be deleted | PENDING |
| Radicale | Complete CRUD/readback, recurrence scopes including qualified this-and-future, alarms, conflict resolutions, offline/restart, deletion and disconnect | PENDING |
| Nextcloud | Same applicable CalDAV matrix, recording server version and scheduling capabilities | PENDING |
| Fastmail | Same applicable hosted CalDAV matrix, including reauthorization behavior | PENDING |
| Public ICS | Subscribe/refresh, retain recurrence and all-day semantics, cache while offline, restart, remote deletion | PENDING |
| Authenticated ICS | Correct credentials work; wrong/removed credentials show an actionable state; keyring/restart and refresh preserve data | PENDING |

## Widget on Omarchy

Install the exact RC archive using
[the widget candidate guide](https://github.com/brdweb/omacalendar-widget/blob/v0.1.0-rc.4/docs/STABLE_ACCEPTANCE.md);
the marketplace/default-branch installation may still select the public beta.

| Check | Expected result | Result |
|---|---|---|
| Install/update/reload | Settings survive beta upgrade and shell reload; plugin reports candidate version | PENDING |
| Four bar edges | Popup placement and every visible action work on top/bottom/left/right | PENDING |
| Mixed monitors/scales | Popup follows the intended monitor, stays within bounds and has usable controls at 1/1.25/2 scale | PENDING |
| Views/filtering/scroll | Calendar filters, today positioning, scrolling and switching views preserve correct selection | PENDING |
| CRUD/undo | Create/edit/move/delete/undo and RSVP converge with the desktop; simultaneous edits expose correct state | PENDING |
| Backend recovery | With daemon stopped, opening the widget activates it; restart/missed revisions reconnect with correct data | PENDING |
| App focus | Open event/new event focuses one desktop instance on the intended monitor | PENDING |
| Remove/reinstall | Removing widget leaves app/data intact; reinstall and settings behavior match documentation | PENDING |

## Flatpak desktop

| Check | Expected result | Result |
|---|---|---|
| Install and launch | Runtime installation completes, launcher opens, version is correct, no missing Qt/QML module | PENDING |
| Isolation | Native and Flatpak can run together with separate data, sockets, windows and keyring identities | PENDING |
| Google/CalDAV | Browser callback, keyring unlock, CRUD and restart work within the sandbox | PENDING |
| Files/notifications | Portal file selection/import/export, external browser links and reminders work | PENDING |
| Lifecycle | Documented daemon/reminder behavior matches closing/reopening the Flatpak | PENDING |
| Update/remove | Installing the next bundle preserves profile; uninstall retains data unless explicitly requested | PENDING |

## Report and promotion

Record failures with a short synthetic reproduction and the expected/actual
result. Preserve crash evidence and the exact executable identity. Do not paste
raw personal logs, callbacks, database contents, or credentials.

Stable promotion requires every applicable `PLAN.md` exit criterion plus the
full owner pass, no unresolved critical/high defect, strict signed-tag checks,
and independent verification of newly built stable artifacts. A completed short
smoke pass alone does not close the full provider matrix. The widget has its
own acceptance and can be promoted independently.
