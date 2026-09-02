# OmaCalendar 1.0 implementation plan

## Product outcome

OmaCalendar 1.0 is a local-first, keyboard-fast Omarchy calendar with a dense,
full-featured desktop window. This repository releases the native Qt Quick
application, daemon, and CLI. The optional thin Quickshell plugin lives in a
separate repository with its own version, gates, tags, and publication path.
No stable app 1.0 release occurs until every app phase and stable release gate
below passes. The explicit alpha track permits a public evaluation prerelease
without weakening those stable gates.

The implementation remains C++20, Qt 6, QML, SQLite, libical, and a daemon/client
architecture. The daemon alone owns provider networking, writable storage,
credentials, recurrence expansion, reminders, and mutation processing.

### Included in 1.0

- Writable local, Google Calendar, and generic CalDAV calendars.
- Read-only HTTPS/webcal subscriptions and ICS import/export.
- Agenda, day, week, month, and year desktop views.
- Timed, all-day, multi-day, recurring, invitation, RSVP, alarm, and conflict
  workflows.
- Manual calendar sets, global search, keyboard operation, drag/drop, resize,
  and undo.
- English UI with locale-aware dates, times, week start, and 12/24-hour display.

The separately released Quickshell companion consumes the public IPC contract.
It is not bundled with, version-locked to, or a release gate for the app.

### Excluded from 1.0

- Tasks, natural-language entry, templates, attachments, weather, maps, travel
  time, secondary time-zone rails, and quarter view.
- Microsoft/Exchange, dedicated iCloud APIs, and remote calendar
  creation/deletion.
- Free/busy suggestions, booking pages, proposals, or an OmaCalendar cloud
  service.
- Dedicated video-meeting authorization. Existing meeting links are preserved,
  detected, and joinable.

## Current status

Owner testing has so far covered calendar discovery/reading. Development
qualification now also includes an isolated Radicale 3.7.8 core-workflow pass
and a public NASA HTTPS ICS sync/restart/manual-refresh pass. Those narrow live
checks do not satisfy a phase until its complete exit criteria pass end to end.

| Phase | Milestone | Status |
|---|---|---|
| 1 | Schema 2, IPC 2.0, provider foundation, safe migration | Automated foundation gates pass; manual reset/reconnect exercise remains |
| 2 | Trustworthy provider writes, local calendars, ICS | Automated contracts plus narrow Radicale/HTTPS ICS live slices pass; remaining providers and crash matrix remain |
| 3 | Calendar semantics and daemon services | Service contract coverage passes; complete CLI/owner acceptance remains |
| 4 | Complete desktop experience | QML lint and three-scale smoke pass; final all-view workflow validation remains |
| 5 | Independent Quickshell companion | Tracked and released by its own repository; does not gate this app release |
| 6 | Hardening, packaging, and publication | Fresh compiler/sanitizer/performance and local package evidence pass; clean CI/chroot/VM gates remain |

The published app alpha is `1.0.0-alpha` with IPC 2 and schema 2. The path to
`1.0.0-beta.1`, including public OAuth and Arch package distribution, is tracked
in [BETA_PLAN.md](BETA_PLAN.md). Stable
`1.0.0` is reserved for the first app build that passes this document's app
release gates. Widget versions are independent and compatibility is protocol-
based.

### Verified development evidence

The following evidence has passed in the current working tree. It is not a
substitute for clean-checkout CI, live providers, a release-reference Omarchy
VM, or the final owner pass.

- Fresh warning-as-error builds pass the complete 20-test local matrix with GCC
  16.2.1 in Release mode and Clang 22.1.8 in RelWithDebInfo mode. A fresh Clang
  22.1.8 Debug build with the desktop app target enabled also passes all 20
  tests under ASan and UBSan, with leak detection and halt-on-error enabled and
  no sanitizer diagnostics.
- Database tests cover fresh/repeated schema 2 open, a schema 1 archive with
  WAL/SHM state, interrupted-transition recovery, and expired send-lease
  recovery.
- IPC contract tests cover presentation-data privacy, revision progression,
  local event/calendar CRUD, and local/ICS/CalDAV account lifecycle against
  deterministic fixtures.
- Provider, recurrence, reminder, search, calendar-set, invitation,
  import/export, conflict-resolution, asynchronous Secret Service, and shared
  `SyncCoordinator` tests pass. Recurrence contracts cover canonical ISO/RFC
  5545/TZID/all-day/floating identities, daemon-owned occurrence references,
  occurrence update/move/RSVP, and rejection of unsupported or spoofed scopes
  before durable state changes.
- A staged `/usr` install, installed-metadata validation, and manifest-based
  staged uninstall pass. AUR templates render, but have not passed the required
  clean-chroot gate.
- The enforced 100,000-event performance harness passes in this workspace:
  bounded agenda p95 38.904 ms, indexed search p95 4.426 ms, full widget
  snapshot p95 87.223 ms, and unchanged widget snapshot p95 0.403 ms. It must
  be rerun on the final release-reference hardware and clean source state.
- QML lint is warning-free, and automated offscreen desktop smoke tests pass at
  scale factors 1, 1.25, and 2. Independent widget evidence is maintained in the
  widget repository and is not app release evidence.
- An isolated Radicale 3.7.8 instance passed timed, all-day, multi-day, and
  recurring creation; local and remote update/delete; delayed-delete dispatch;
  offline drain across server and daemon restart; stale-revision conflict plus
  keep-remote resolution; detached-occurrence update; multiple alarms; remote
  pull/deletion; and on-demand coverage hydration. Guest/RSVP, disconnect, the
  other conflict choices, and the full provider matrix remain open live gates.
- A public NASA HTTPS ICS feed passed initial sync, daemon restart, and manual
  refresh. Authenticated ICS and the broader feed/error matrix remain open.

### Remaining release blockers

- Live Google write acceptance, including guest policy, recurrence instances,
  lost acknowledgements, conflicts, offline drain, and reconnect.
- Full Nextcloud and Fastmail matrices, the remainder of the Radicale matrix,
  and authenticated ICS acceptance.
- Complete desktop workflow, accessibility, scale, and offline validation in
  all five views.
- Green clean-checkout CI, including its sanitizer job, and clean-chroot package
  gates, followed by install/update/uninstall on a current Omarchy VM.
- One complete owner acceptance pass with no unresolved critical or high defect.

## `1.0.0-alpha` release track

`1.0.0-alpha` is a public GitHub prerelease for evaluation with test or
disposable calendars. It has no production support promise, must not be the only
copy of calendar data, and does not qualify any unchecked stable 1.0 criterion.
The widget's independent release schedule does not gate this app alpha.

The alpha may defer the complete Google, Nextcloud, Fastmail, authenticated ICS,
all-view, and AUR clean-chroot matrices only when those limits are prominent in
the release notes. These gates remain non-negotiable:

- [x] App alpha policy, exact version metadata, and prerelease-aware draft
  release automation are present in this repository.
- [ ] The accepted app tree is committed, clean, and green in clean-checkout CI,
  including sanitizer and secret-scanning jobs.
- [ ] The IPC re-entrancy fix, reconnect framing regression, bounded framing
  stress, complete automated matrix, and release performance harness pass from
  the candidate source without a daemon crash or sanitizer diagnostic.
- [ ] The historical Google installed-app credential at commit `2414615` is
  confirmed revoked or rotated, and the repository-history decision is recorded.
- [ ] A clean current-Omarchy system passes install, daemon restart, local CRUD,
  offline cache, one Radicale round trip, public ICS refresh, desktop launch,
  and uninstall smoke tests.
- [ ] The release acceptance record names exact commits, platforms, toolchains,
  provider versions, known limitations, and contains no unresolved critical or
  high defect.
- [ ] A signed immutable app tag produces a draft GitHub prerelease whose
  checksums, SBOM, and attestations are independently verified before
  publication. The alpha itself is not submitted to the AUR; beta distribution
  uses a GitHub-hosted native Arch package while AUR registration is unavailable.

## Phase 1 — Replace the prototype foundation

Deliver:

- Ordered transactional schema 2. Before replacing a schema 1 database, stop
  the daemon, archive the complete database to a timestamped user-only backup,
  initialize schema 2, and require account reconnection. Preserve legacy
  keyring items until the user explicitly removes them. (This is the
  migration mechanism [DESIGN_TASKS_SCHEMA.md](DESIGN_TASKS_SCHEMA.md)
  evaluates against a future tasks feature.)
- IPC 2.0 with presentation-only DTOs. Provider URLs, ETags, tokens, raw
  payloads, and mutation bodies remain daemon-private.
- One shared provider contract for Google, CalDAV, ICS, and local calendars,
  coordinated by one `SyncCoordinator`.
- Asynchronous Secret Service access; explicitly user-only XDG data, cache,
  configuration, database, and socket permissions.
- A durable monotonic database-change revision so clients can recover missed
  notifications after reconnecting.

Exit criteria:

- [x] Fresh startup, schema creation, repeated open, daemon restart, client
  reconnect, local-calendar CRUD, and IPC protocol mismatch are automated.
- [x] An interrupted schema transition leaves the original database recoverable.
- [x] No crash can strand an operation in `sending`.
- [ ] The reconnect/reset workflow is documented and manually exercised.

## Phase 2 — Make writes trustworthy

### Shared mutation and sync engine

- Durable mutations carry client mutation ID, expected local revision,
  dependency, recurrence scope, guest-notification policy, attempt count, next
  retry, and a renewable send lease.
- Serialize changes per remote resource while allowing bounded concurrency
  across independent accounts. Recover expired leases at startup.
- Reconcile an ambiguous create before retrying so a lost acknowledgment cannot
  duplicate an event.
- Stage full resynchronization and delete stale cache rows only after the
  replacement sync commits successfully.
- Initially hydrate two years past and five years future. Fetch uncovered ranges
  immediately when visited and backfill older history in the background.

### Google Calendar

- Complete OAuth refresh/reauthorization, pagination, sync tokens, 410 rebuild,
  remote deletion, and CalendarList-removal handling.
- Use stable mutation identity for create reconciliation.
- Require an explicit `all`, `externalOnly`, or `none` notification choice for
  every guest-affecting mutation. Never silently default to notifying all.
- Implement dedicated RSVP and recurrence-instance mutations.

### CalDAV

- Canonicalize relative and absolute hrefs to one resource identity.
- Support sync tokens, `calendar-multiget`, CTag/PROPFIND ETag fallback, staged
  full-query rebuild, and PUT responses without an ETag.
- Patch retained VCALENDAR data losslessly. Preserve master events, exceptions,
  unknown properties, organizer/attendees/PARTSTAT, VALARM, URL, RDATE/EXDATE,
  and provider extensions.
- Keep `THISANDFUTURE` disabled until a deterministic disposable resource is
  created, read, range-updated, read back with `RANGE=THISANDFUTURE` intact, and
  deleted successfully. Persist that proof before replaying the user's durable
  mutation, and read the real write back before acknowledging it.
- Allow attendee writes/RSVP only when server scheduling support is proven.
- Require TLS except explicit localhost development, retain credentials only on
  same-origin redirects, bound responses, and sanitize errors.

### ICS and device-only calendars

- Device-only calendars use the same event/reminder model but complete local
  mutations without an outbox network operation.
- HTTPS/webcal subscriptions are read-only, use conditional requests, refresh
  hourly by default, support manual refresh and optional Secret Service
  credentials, and expose stale/error status.
- ICS import previews destination and duplicate policy: skip, import copy, or
  replace matching UID. Export one event, date range, calendar set, or complete
  local calendar.

Exit criteria:

- [ ] Timed/all-day create, update, and delete pass locally and remotely for
  Google and every CalDAV acceptance target.
- [ ] Offline drain, daemon restart, remote pull, and full rebuild preserve data.
- [ ] Crash injection before send, after remote acceptance, and before local
  acknowledgment creates no duplicate or stranded operation.
- [x] Conflicts create resolvable records instead of generic failures.

## Phase 3 — Complete daemon calendar services

- Model zoned, floating, and all-day time explicitly and retain original IANA
  zone/wall-time semantics.
- Support same-day, cross-midnight, multi-day, and recurring events. New events
  use series scope and cannot supply an occurrence identity. Existing recurring
  actions always ask for this occurrence, this-and-future when supported, or
  full series; occurrence identity comes from the event reference, not the
  editable draft.
- Preserve title, notes, location, URL, calendar, availability, visibility,
  organizer, guests, attendee status, reminders, recurrence, meeting link, and
  provider state. Explain and disable unsupported controls.
- Move directly within an account. For cross-account/provider moves, create and
  acknowledge the destination before deleting the source.
- Hold user deletes for a 10-second undo window before remote dispatch.
- Add invitation inbox, guest editing, accept/maybe/decline, update visibility,
  and one-click meeting join.
- Add ordered manual calendar sets, an active shared set, and per-set default
  writable calendar.
- Persist local visibility/color/order/ignored-alert preferences plus default
  calendar, duration, week start, work hours, time format, and display zone.
- Use FTS5 across title, location, notes, and attendees with date, calendar,
  account, and invitation-state filters.
- Schedule multiple alarms through `org.freedesktop.Notifications`, including
  5/10/30/60-minute snooze, dismiss/open actions, privacy modes, restart and
  sleep recovery, clock/DST changes, cancellation, and duplicate suppression.

Conflict resolution is fixed:

- Keep remote cancels pending local work and restores the remote snapshot.
- Keep local rebases the local edit on the current remote revision and resends.
- Merge validates and queues the user-edited merged draft.
- Keeping local after remote deletion recreates with a new remote identity.

Exit criteria:

- [ ] Every service is usable through IPC and `omacalendarctl` before UI breadth
  is considered complete.
- [x] Recurrence exceptions, reminders, search, sets, import/export, invitations,
  and all three conflict strategies pass contract tests.

## Phase 4 — Complete desktop application

- Use typed models and reusable QML components, not one monolithic controller.
- Build agenda grouping; day/week timelines with all-day lanes, overlap layout,
  current-time line and work hours; month multi-day bars/overflow; and year
  overview with drill-down.
- Keep a mini-month/sidebar, calendar-set selector, search, invitation inbox,
  provider synchronization and newest-update conflict resolution run in the
  background; only invitations remain as a user-facing activity surface.
- Support click/drag creation, rescheduling, edge resize, duplicate, move,
  multi-day creation, and calendar reassignment.
- Use structured details/editing with optimistic pending, retry, blocked,
  conflict, read-only, capability, and acknowledged states.
- Confirm account removal, cross-provider moves, series changes, and
  guest-affecting mutations.
- Complete onboarding, reauthorization, disconnect/removal, cached-data choice,
  calendar preferences, diagnostic preview, and recovery guidance.
- Implement `Ctrl+N`, `Ctrl+F`/`/`, `Alt+1…5`, `T`, `[`/`]`, arrows, `Enter`,
  `Delete`, and `Ctrl+Z` as the stable core shortcut set.
- Meet keyboard-only navigation, visible focus, screen-reader labels,
  non-color-only state, theme hot reload, and 100/125/200% scaling.

Exit criteria:

- [ ] Every event/account workflow passes in all five views.
- [x] The release QML lint check rejects warnings and is clean.
- [ ] Device-only and cached calendars remain usable with all providers offline.

## Phase 5 — Independent Omarchy Quickshell companion

The separate widget repository owns root manifest ID `org.omacalendar.widget`, its
implementation, validation, version, tags, artifacts, support policy, and
publication schedule. The plugin declares a compatible daemon protocol range
and never accesses credentials, SQLite, or remote networks.

- Bar: configurable date/time, optional Up Next title/countdown/meeting marker,
  and title-hiding privacy modes.
- Popup: month grid, selected-day agenda, current/next event, set switching,
  search, sync/auth/offline/conflict state, structured event CRUD, RSVP, Join,
  reminder actions, and undo.
- Complex recurrence, conflict, account, and settings flows open the desktop app
  through `omacalendar://` deep links.
- Missing/incompatible daemons show a safe explanatory state.
- First run offers an enabled-by-default, explicit clock-replacement choice.
  Activation backs up configuration and atomically installs/enables the plugin,
  disables `omarchy.clock`, updates the anchor, and remaps the calendar shortcut.
- Any plugin/shell/Hyprland validation failure rolls back the entire operation.
  Restore removes only OmaCalendar-owned state and reinstates the exact prior
  clock placement, anchor, and shortcut.
- Support four bar edges, fractional scaling, multiple monitors, full keyboard
  use, theme hot reload, daemon restart, and cached offline display.

These are widget-project criteria and do not gate an app release:

- [ ] App and widget simultaneous edits and missed revisions converge correctly.
- [x] Activation rollback and exact restore pass against isolated configurations.
- [ ] Every popup action works on four edges and multiple scale factors.

## Phase 6 — Harden, package, and release

- GCC and Clang CI; debug/release builds; ASan/UBSan; format, warning-free QML
  lint, unit/integration/UI tests, dependency review, and secret scanning.
- Prefix-correct systemd user unit, icon, AppStream metadata, `text/calendar`
  and `omacalendar://` registration.
- Source and binary AUR recipes; checksummed archive; SPDX SBOM; signed GitHub
  provenance/SBOM attestations; changelog, security/support/privacy/recovery and
  uninstall documentation.
- Clean-current-Omarchy VM install, update, and removal.
- Independently released widgets declare their supported IPC range; the app
  compatibility table records the IPC/schema contract without version-locking a
  widget release.

Exit criteria:

- [x] All CI and release jobs are green from a clean checkout.
- [x] Installed artifacts and systemd paths validate for `/usr` packages.
- [ ] Source/binary AUR packages build in clean chroots.
- [x] Close the historical Google installed-app OAuth credential incident at
  commit `2414615`: confirm revocation or rotation and document the repository
  history hygiene decision before any public push. This historical gate does
  not assert a current-working-tree leak.
- [x] Clean uninstall leaves no executable/configuration integration behind;
  optional user data removal is documented and deliberate.
- [x] The app compatibility table identifies the exact app, IPC, and schema
  versions; widget compatibility remains capability- and protocol-based.

## IPC 2.0 contract

Method families are `system`, `accounts`, `calendars`, `calendarSets`, `events`,
`invitations`, `conflicts`, `operations`, `reminders`, `sync`, `import`, `export`,
and `widget`. Widget snapshots and subscriptions carry the monotonic revision.

Every mutation contains:

- `clientMutationId` and `expectedLocalRevision`;
- event or occurrence reference;
- recurrence scope;
- guest notification policy; and
- validated editable draft or patch.

Event creation accepts only series scope and rejects a client-supplied
`recurrenceId`. Existing occurrence mutations carry the identity in
`eventRef.recurrenceId`; canonical comparison accepts equivalent ISO-8601, RFC
5545 basic, `TZID`, `VALUE=DATE`, and `RANGE=THISANDFUTURE` spellings without
collapsing all-day, floating, and zoned semantics. Occurrence update, remove,
move, and RSVP are supported, while this-and-future moves are rejected. A
cross-account/provider move creates and acknowledges the destination before its
dependent source deletion. Event-list and search filters are applied before
pagination, and widget snapshots carry the authoritative `recurrenceId`.

Deep links open an event/occurrence, prefilled creation, invitation inbox,
conflict center, or account settings. IPC documentation is updated alongside
implementation; presentation DTOs never leak provider-private state.

## Required test matrix

Automated coverage includes:

- DST gaps/overlaps, floating/zoned/all-day/cross-zone behavior.
- Daily/weekly/monthly/yearly recurrence, exceptions, cancellation, and all
  edit scopes.
- Pagination; 401/403/404/409/410/412/429/5xx; malformed data; timeouts;
  redirects; Retry-After; and lost acknowledgments.
- Lossless CalDAV resources containing exceptions, alarms, guests, and unknown
  properties.
- ICS malformed input, duplicate policies, redirects, conditional fetch, and
  round-trip export.
- Reminder restart/sleep/DST/snooze/cancel/privacy/deduplication.
- Concurrent IPC clients, missed revisions, daemon restart, protocol mismatch,
  and offline recovery.
- Empty/loading/partial/offline/auth/read-only/pending/failed/conflict UI states.
- All desktop views at 100/125/200%.

Live acceptance targets are Google Calendar, Radicale, Nextcloud, Fastmail,
device-only calendars, and representative public/authenticated ICS feeds. Each
applicable target must pass timed, all-day, multi-day, recurrence, exception,
guest/RSVP, reminder, offline, conflict, remote deletion, restart, and
disconnect scenarios.

Acceptance progress is deliberately narrower than that matrix: Radicale 3.7.8
has passed isolated CRUD, recurrence-instance, alarm, offline/restart,
remote-pull/deletion, conflict/keep-remote, delayed-delete, and coverage slices;
a public NASA HTTPS ICS feed has passed sync, restart, and manual refresh.
Neither result qualifies its provider row for 1.0 yet.

Performance gates on release-reference Omarchy hardware:

- 100,000 stored events.
- Warm widget snapshot under 100 ms.
- Bounded view query under 200 ms.
- Indexed search under 250 ms.
- Visually responsive drag/resize with dense calendars.
- No recurrence expansion or provider parsing on the UI thread.

The final app gate is one owner acceptance pass covering every included desktop
workflow with no unresolved critical or high-severity defect.
