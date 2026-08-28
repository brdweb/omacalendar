# OmaCalendar implementation plan

## Outcome

Build a dependable, local-first calendar platform for Omarchy with two clients:

1. a native Qt Quick calendar application that owns account and product
   settings; and
2. a small Omarchy Quickshell bar widget for immediate schedule access.

This work pauses after Phase 4 so the project owner can test the Google OAuth
and Calendar integration with their own credentials. The shell widget and
public packaging follow after that checkpoint.

## Working status

| Step | Milestone | Status |
|---|---|---|
| 1 | Repository, architecture, build, MIT license, local daemon and IPC | Implemented |
| 2 | SQLite calendar core, recurrence, offline outbox, conflicts and provider framework | Implemented for the checkpoint; performance hardening continues before release |
| 3 | CalDAV vertical slice and theme-aware agenda/month desktop app | Implemented for the checkpoint; live compatibility plus multiget/ETag optimization remain |
| 4 | Google desktop OAuth, Calendar API mapping/incremental sync and safe credential handoff | Implementation complete; owner credential acceptance test is the checkpoint |
| 5 | Fantastical-style interaction depth and Omarchy Quickshell widget | Planned after owner acceptance |
| 6 | Packaging, GitHub release engineering and Omarchy submission | Planned |

## Scope through the Phase 4 checkpoint

### Included

- Multiple CalDAV and Google accounts.
- Multiple calendars per account.
- Offline-first agenda and month queries, with a core API suitable for later
  day and week timelines.
- Create, edit, and delete timed and all-day events.
- Recurrence rules and recurrence exceptions.
- Attendees, organizer state, RSVP state, reminders, locations, descriptions,
  and provider conference links where they round-trip safely.
- Shared settings/calendar-set storage for the later app and widget controls.
- IPC/CLI search, account status, manual sync, and actionable errors.
- Background synchronization and an offline mutation outbox.
- Omarchy theme following in the desktop app.
- A stable local IPC protocol for the later Quickshell widget.

### Deferred until after the checkpoint

- Omarchy Quickshell widget implementation and marketplace submission.
- Day/week timeline views, calendar-set management UI, and richer settings.
- Deterministic natural-language quick entry with an editable preview.
- Tasks, weather, travel time, contacts lookup, booking pages, proposals,
  mirroring, and automatic location-based calendar sets.
- Mobile, Windows, and macOS packaging.
- Google push notifications; desktop polling and incremental sync are used
  first because Google watch channels require a public HTTPS receiver.
- Full natural-language parity with Fantastical. The first parser is a
  deterministic, previewed subset.

## Non-goals

- No Microsoft Graph, EWS, Outlook.com, or Microsoft 365 support.
- No cloud service operated by the OmaCalendar project.
- No plaintext token or password storage.
- No network provider code inside the Omarchy shell process.
- No silent last-write-wins behavior for detected remote conflicts.

## Architecture

The daemon is the only process that accesses remote providers or the writable
database. Clients communicate over a user-only local socket with a versioned,
newline-delimited JSON protocol.

```text
Google Calendar API ----\
                         +--> omacalendard --> SQLite cache + outbox
CalDAV servers ---------/          |
                                   +--> Secret Service
                                   +--> local notifications
                                   +--> local JSON IPC
                                            |
                                  +---------+----------+
                                  |                    |
                            desktop app       Quickshell widget
                                               (Phase 5)
```

Detailed boundaries and data ownership are documented in
[`ARCHITECTURE.md`](ARCHITECTURE.md). The wire contract is documented in
[`IPC.md`](IPC.md).

## Phase 1 — Repository and technical foundation

### Deliverables

- MIT license, contribution-ready repository structure, architecture records,
  and dependency policy.
- CMake build with warnings enabled and Qt Test integration.
- Core value types for accounts, calendars, events, attendees, alarms, and
  provider capabilities.
- SQLite database wrapper with ordered migrations and transaction helpers.
- Versioned IPC server/client with request, response, notification, reconnect,
  and maximum-message-size handling.
- Status-oriented logging with event content and credentials excluded by
  default.

### Exit criteria

- A fresh database migrates to the current schema and re-opening it is
  idempotent.
- CRUD and date-range queries pass automated tests.
- Daemon responds to `system.ping` and `system.info` over the local socket.
- Client reconnection after daemon restart is covered by a test.
- No secret value is represented by a database column or logged field.

## Phase 2 — Calendar core, offline outbox, and synchronization framework

### Deliverables

- Canonical event model that preserves original time zone identifiers and
  treats all-day values as dates rather than midnight timestamps.
- RFC 5545 parsing/serialization and recurrence expansion through libical.
- Indexed ordinary-event queries plus bounded recurrence expansion for agenda
  and calendar views; a persisted occurrence cache remains a release
  optimization.
- Outbox state machine: `pending`, `sending`, `retry_wait`, `blocked`, `done`.
- Idempotency keys, exponential backoff with jitter, retry classification, and
  a visible dead-letter state.
- Provider interface with full sync, incremental sync, create, update, delete,
  capability discovery, and authentication-state reporting.
- Conflict detection using remote ETags/change identifiers and explicit
  conflict records.
- Settings and calendar-set repositories shared by all future clients.

### Exit criteria

- Local writes succeed while offline and generate exactly one outbox entry.
- A retry cannot create a duplicate local event or duplicate queued mutation.
- Recurrence tests cover daily/weekly/monthly/yearly rules, exclusions,
  exceptions, DST boundaries, leap days, and floating/all-day values.
- Simulated provider responses exercise retryable, authentication, permission,
  conflict, and terminal failures.
- A database containing at least 10,000 events remains responsive for bounded
  date-range and text-search queries.

## Phase 3 — CalDAV vertical slice and usable desktop application

### CalDAV deliverables

- Current-user-principal and calendar-home-set discovery.
- Calendar collection discovery with names, colors, privileges, CTags, and sync
  tokens when advertised.
- Initial bounded `calendar-query` synchronization; `calendar-multiget` is a
  planned scale optimization.
- RFC 6578 `sync-collection` incremental synchronization with a successful
  full-query rebuild when the token is unsupported or expired. An ETag-list
  fallback remains a pre-release optimization.
- Event PUT/DELETE with `If-Match`/`If-None-Match` and conflict recording.
- HTTPS required by default, with an explicit localhost development exception.
- Basic authentication stored in Secret Service. Provider-specific app-password
  instructions remain documentation, not hard-coded behavior.
- Test matrix for Radicale, Nextcloud, and at least one hosted CalDAV service.

### Desktop app deliverables

- First-run screen, account settings, calendar list, and synchronization status.
- Agenda and month views designed to establish the shared visual language for
  later day/week timelines.
- Event details and editor for core fields.
- Account management and the storage/API foundation for calendar sets, default
  calendar, week start, work hours, and default duration.
- Optimistic local writes with a pending indicator; interactive conflict
  resolution is part of the post-checkpoint UX pass.
- Theme adapter that watches Omarchy's active `colors.toml` and `shell.toml`,
  with a safe fallback palette outside Omarchy.
- Keyboard navigation, focus states, scalable typography, and non-color-only
  status indicators.

### Exit criteria

- A user can add a CalDAV account in the app and perform bidirectional event
  CRUD without restarting either process.
- Offline edits drain after reconnection and provider conflicts are visible.
- App starts and displays cached data with every provider unreachable.
- The principal views are usable at 100%, 125%, and 200% scaling.
- Theme changes are reflected without restarting the app.

## Phase 4 — Google Calendar integration and credential test checkpoint

### Deliverables

- Installed-application OAuth authorization-code flow with PKCE and a random
  loopback port opened in the system browser.
- Project-owned public desktop client ID embedded for one-click browser sign-in;
  an optional developer override remains available in code, while refresh tokens
  and any override secret are stored only in Secret Service.
- Narrow Google Calendar scopes sufficient for calendar-list and event CRUD.
- CalendarList synchronization and per-calendar event synchronization.
- Full sync followed by `syncToken` incremental sync, pagination, deleted-event
  handling, and HTTP 410 full-resync recovery.
- Google event mapping for all-day/timed events, recurrence and exceptions,
  attendees, reminders, visibility, transparency, status, and conference data
  pass-through.
- Token refresh, revoked-consent, permission, quota, rate-limit, and malformed
  response handling.
- Account disconnect that revokes or forgets credentials and stops sync safely.
- A credential test guide that never asks the user to paste secrets into logs,
  chat, command history, or committed files.

### Exit criteria

- The app completes browser sign-in and discovers at least two calendars.
- Remote create, update, and delete changes appear locally through incremental
  sync; local equivalents appear remotely.
- Timed, all-day, recurring, and exception events round-trip in the test matrix.
- Expired access tokens refresh without user interaction.
- HTTP 410 causes a bounded clean rebuild of only the affected calendar.
- Disconnecting removes keyring credentials and leaves cached-data behavior
  explicit to the user.
- Automated checks confirm no token or client secret is written to SQLite or
  ordinary logs.

At this point development pauses for owner testing with real Google credentials.

## Phase 5 — Omarchy Quickshell widget

The permanent plugin uses a third-party namespaced ID and a root
`manifest.json`. It is a thin IPC client and contains no provider credentials,
database access, or remote networking.

Planned panel features:

- Date/time bar label replacing `omarchy.clock` by explicit user choice.
- Month grid with event marks and a selected-day agenda.
- Up-next summary, calendar-set switcher, and sync state.
- Quick event entry, simple edit/delete/RSVP, and app handoff for complex edits.
- Native `qs.Commons.Color`, `Style`, and `Border` usage.
- Keyboard navigation and support for all bar positions and multiple monitors.

The same phase deepens the desktop experience with day/week timelines,
calendar-set controls, search, keyboard-first navigation, and a deterministic
natural-language quick-add parser. Parsing always produces a visible preview;
ambiguous dates, calendars, recurrence, or invitees require confirmation.

## Phase 6 — Hardening, packaging, and publication

- AUR source and binary packages, systemd user unit, desktop entry, icons, and
  uninstall behavior.
- GitHub Actions for builds, unit/integration tests, sanitizers, `qmllint`,
  manifest validation, release archives, checksums, and dependency review.
- Public privacy policy and Google OAuth verification material.
- Security policy, threat model, changelog, support matrix, and recovery guide.
- Separate thin plugin repository for Omarchy marketplace distribution.
- Marketplace preview, README, license, root manifest, and submission issue.

## Quality strategy

### Automated test layers

- Pure unit tests for mapping, recurrence, time zones, XML/JSON parsing, retry
  policy, and conflict decisions.
- Repository tests against temporary SQLite databases.
- IPC contract tests with fragmented messages, invalid JSON, oversized input,
  reconnects, and protocol mismatch.
- Provider contract tests using sanitized recorded fixtures and a deterministic
  local HTTP server.
- Live-provider smoke suites that run only with explicitly supplied local test
  accounts.
- QML model and navigation tests, followed by screenshot regression tests once
  the visual system stabilizes.

### Required edge cases

- DST gaps and overlaps in several IANA zones.
- Events crossing midnight or spanning multiple days.
- All-day events across time zones.
- Recurrence edits to one occurrence and an entire series.
- Deleted recurrence exceptions.
- Duplicate UIDs in different calendars.
- CalDAV servers without sync-token support.
- Google pagination, token expiry, 410, 412, 429, and transient 5xx responses.
- Network loss between remote mutation success and local acknowledgement.
- Database migration interruption and daemon crash recovery.

## Security and privacy requirements

- User-only runtime socket under `XDG_RUNTIME_DIR`.
- Strict protocol framing and message-size limits.
- No shell execution from calendar content.
- No HTML event-description rendering without sanitization.
- TLS verification enabled; insecure remote CalDAV endpoints rejected.
- Credentials sent to `secret-tool` through standard input, never command-line
  arguments.
- Logs use local object IDs and provider status codes, not titles,
  descriptions, attendee addresses, or tokens.
- Exported diagnostics require an explicit preview before sharing.
- Dependency licenses and release artifacts are inventoried in CI.

## Release compatibility policy

- Semantic versioning for app and plugin releases.
- Integer IPC protocol major version with additive minor capability discovery.
- Database migrations are forward-only and transactional; downgrade is not
  promised without restoring a backup.
- The widget declares minimum/maximum tested daemon protocol versions.
- The initial public release targets current Omarchy stable and current Arch
  Qt packages, with an explicit tested-version table in the README.
