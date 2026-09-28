# Local IPC protocol 2.1

## Transport and trust boundary

OmaCalendar uses newline-delimited UTF-8 JSON on the user-only local socket
`$XDG_RUNTIME_DIR/omacalendar/daemon.sock`. The runtime directory and socket are
created with owner-only permissions. Both peers enforce a 1 MiB frame limit;
large event collections are bounded and paginated.

In packaged installations, `omacalendard.socket` owns this endpoint and passes
its listening descriptor to `omacalendard`. Connecting to the socket therefore
starts the daemon on demand even when the desktop application is closed. Socket
activation only opens the local cache; remote provider sync is not required to
serve the initial widget snapshot.

The daemon is the trust boundary. Client DTOs never include credentials,
provider endpoints, remote identifiers, ETags, sync tokens, retained provider
payloads, or queued mutation payloads. Errors are sanitized before crossing
IPC.

## Versioning

Every request carries `protocolMajor: 2`. `protocolMinor` is optional request
metadata and does not participate in routing: requests that omit it, send `0`,
or send an unknown value remain compatible when the major is `2`. A different
major is rejected with `incompatible_protocol`. The daemon advertises
`protocolMinor: 1` through `system.info`; clients discover additive fields and
methods from that response rather than requiring an exact minor match.

```json
{"id":"8aee...","protocolMajor":2,"method":"system.health","params":{}}
```

A successful response contains `result`; a failed response contains a stable
error code, safe message, and retryability flag.

```json
{"id":"8aee...","result":{"ok":true,"revision":42}}
```

```json
{"id":"8aee...","error":{"code":"invalid_params","message":"A bounded date range is required","retryable":false}}
```

## Revisions and subscriptions

Every durable change increments a monotonic database revision. Notifications
are hints, not a source of truth:

```json
{"event":"events.changed","data":{"calendarIds":["..."],"revision":43}}
```

Clients call `system.subscribe` with their last observed `sinceRevision`. A
`catchUpRequired` response makes the client re-query its presentation models.
Clients also re-query after reconnect or daemon restart.

Subscription state belongs to one socket connection and is cleared when that
connection closes. Before a successful subscription the socket receives only
its request/protocol responses, not asynchronous domain notifications. Omitting
`topics` subscribes to `*` for IPC 2.0 client compatibility; an explicit list
replaces the connection's prior list and may contain `*`, a family such as
`events`, a family wildcard such as `events.*`, or an exact event such as
`events.changed`.

## Method surface

### System

- `system.ping`, `system.info`, `system.health`, `system.subscribe`

### Accounts

- `accounts.list`, `accounts.update`, `accounts.reauthorize`,
  `accounts.disconnect`, `accounts.remove`, `accounts.test`
- `accounts.addGoogle` (`google.oauthStart` remains a development alias)
- `accounts.addCalDav` (`accounts.createCalDav` is a development alias)
- `accounts.addIcs`
- `google.configureClient`, `google.oauthStart`, `google.oauthCancel`,
  `google.disconnect`

### Calendars and calendar sets

- `calendars.list`, `calendars.updatePreferences`
- `calendars.probeThisAndFuture` with `{calendarId}` for a writable CalDAV
  calendar whose recurrence-range support is not yet proven
- `calendars.upsert` for writable device-only calendars
- `calendars.remove` for confirmed custom device-only calendars and owned,
  non-primary Google calendars; built-in, primary, shared, and read-only
  calendars are protected
- `calendarSets.list`, `calendarSets.upsert`, `calendarSets.remove`,
  `calendarSets.activate`
- `settings.get`, `settings.set`, `settings.getMany`

`settings.getMany` is an additive, not yet released method that reads several
settings in one request; the protocol minor advances when it first ships. It
takes `keys`, an array of 1 to 64 setting names, and an optional `fallbacks`
object mapping a key to the value returned when that key is unset. The result is `{"values": {...}}` with one entry per requested key,
resolved exactly as `settings.get` resolves it. Clients find it in
`system.info` `methods`; a daemon without it answers `method_not_found`, and
clients fall back to `settings.get`.

`calendars.probeThisAndFuture` creates a temporary test resource on the selected
server, checks that `RANGE=THISANDFUTURE` survives a write and readback, and
removes the resource before enabling the capability. It does not edit an
existing user event. The response contains `calendarId` and `state` (`checking`
or `supported`). A `checking` response completes asynchronously through
`calendars.changed`; already-proven support returns immediately.
Read `capabilities.thisAndFutureProbeState` and
`capabilities.thisAndFutureProbeMessage` from the refreshed calendar. Terminal
probe states are `supported` and `failed`. A failed
probe also surfaces through `sync.status`. Busy or still-loading credentials
can require a retry. Recurrence writes remain guarded until successful proof
and cleanup; a started check is not proof of server support.

### Events and invitations

- `events.list`, `events.get`, `events.search`
- `events.create`, `events.update`, `events.remove`, `events.move`,
  `events.respond`, `events.undo`
- `invitations.list`, `invitations.markSeen`
- `contacts.suggest`
- `freebusy.query`

`freebusy.query` is additive and not yet released (the protocol minor remains
2.1). It takes ISO `start` and `end` at most 8 days apart and up to 20
`attendees` addresses, plus optional `calendarId` (whose Google account is used
for remote lookups) and `excludeEventId`/`excludeRecurrenceId` (the event being
edited, which does not count as busy). It answers at once with `requestId`,
`start`, `end`, `self` (the user's merged busy intervals, computed locally),
`attendees` (address → busy intervals already known: the user's own addresses
and five-minute cached answers), `pending` (addresses sent to Google) and
`unavailable` (`[{email, reason}]`, where reason is `unsupported` when no
Google account can answer). When Google replies, an `events.freeBusy`
notification carries the same `requestId` with the remaining `attendees` and
`unavailable` entries (`notFound` for people Google will not share,
`permission` when the account's grant does not cover free/busy). Intervals are
`{start, end}` in UTC.

`contacts.suggest` is additive and not yet released. It takes a non-empty
`prefix` (at most 200 characters) and an optional `limit` (1 to 25, default 8)
and returns `{"prefix": ..., "contacts": [{"email", "displayName"}]}`: distinct
guest and organizer addresses from cached, non-deleted events whose address
starts with the prefix or whose name contains it, case-insensitively, most
used first. It reads only the local cache.

`stats.dailyCounts` is additive and not yet released (added after 2.1,
advertised in the next release). Discover it in `system.info` `methods`; the
protocol minor remains 2.1 until that release. It takes required ISO dates
`start` and exclusive `end` (at most 366 days apart), optional `calendarIds`
(same scope as `events.list`; omitted means all calendars), and optional
`timeZone` (IANA ID; defaults to the configured display zone, then the
machine-local zone). It returns `counts` and `coverage` in one object:

```json
{"counts":{"2026-09-02":1},"coverage":{"complete":true,"hydrationScheduled":false,"uncoveredCalendarIds":[]}}
```

Zero-count days are omitted. The `coverage` object has exactly the same fields
as `events.list` (including `rangeTooLarge` when applicable).
Counts are a local-cache snapshot: when `complete` is false,
Google or CalDAV range hydration is still pending and counts may be partial.
Subscribe to `events.changed` and refetch `stats.dailyCounts` when hydration
completes; a successful range hydration emits that notification even if no
events changed. Timed events count on every day touched by their half-open
interval; all-day events count from `startDate` through the day before their
exclusive `endDate`. Recurrences and detached occurrences use the same
expansion as `events.list`. Invalid dates, ranges and time zones return
`invalid_params`.

The yearly statistics read allows up to 50,000 occurrences and 500,000
recurrence expansion steps (rather than the smaller agenda-query budget).
Exceeding either limit returns `database_error` instead of partial counts.

`invitations.list` returns invitations that need a response, sorted upcoming
first. IPC 2.1 additively introduces the `upcomingTotal` and `pastTotal` result
fields alongside `total`. These bucket counts describe the full filtered result,
so clients can summarize the invitation backlog without paging through it.
Timed invitations move to the past bucket at `endUtc`; all-day invitations move
at machine-local midnight on their exclusive `endDate`. IPC 2.0 clients may
ignore the additional fields.

Durable `events.create`, `events.update`, `events.remove`, `events.move`, and
`events.respond` requests use `clientMutationId`, `expectedLocalRevision`,
`recurrenceScope`, and `guestNotificationPolicy`. Existing-event writes also
carry `eventRef {eventId, recurrenceId?}`. Guest-affecting writes reject an
omitted notification policy. `events.undo` instead consumes the returned
`undoToken` with a new `clientMutationId`.

`events.create` accepts only `recurrenceScope: "series"`. Its editable draft
must not contain `recurrenceId`: occurrence identities are provider/daemon-owned
references, not fields a client may invent while creating an event.

For recurring events, `recurrenceScope` is `series`, `occurrence`, or `future`.
An occurrence update, remove, move, or RSVP supplies the occurrence identity in
`eventRef.recurrenceId`; an editable draft cannot replace that identity.
`events.move` supports `series` and `occurrence`, but rejects `future` because a
cross-calendar this-and-future move cannot be represented safely across the
supported providers. Other `future` mutations are exposed only when the target
calendar advertises proven `thisAndFuture` support.

Recurrence references may be returned in ISO-8601 form, RFC 5545 basic form,
all-day date form, or with `TZID`, `VALUE=DATE`, and
`RANGE=THISANDFUTURE` parameters. The daemon compares equivalent spellings
canonically while preserving the distinction between all-day, floating, and
zoned time. Clients should round-trip the `recurrenceId` from a presentation DTO
instead of synthesizing one from the displayed start time. `events.get` accepts
an optional `recurrenceId` (directly or in `eventRef`) and returns the detached
or generated occurrence rather than the series master.

`events.get` additively returns `attachments` (not yet released; the protocol
minor remains 2.1): `[{title, url, mimeType}]` read on demand from the stored
Google event or iCalendar `ATTACH` properties of that occurrence, falling back
to the series master. Only HTTP(S) links are listed; inline binary attachments
are omitted. `events.list` does not carry attachments.

`events.create` and `events.update` additively accept `addConference: true` on
the event (not yet released) to create a Google Meet link. It applies only to
Google calendars whose `capabilities.conferenceProperties.allowedConferenceSolutionTypes`
include `hangoutsMeet`; other calendars answer `conference_unsupported`. It is
ignored when the event already has a `conferenceUrl`. The daemon generates the
conference request id itself and keeps it with the queued write until Google
accepts it; the new link arrives as `conferenceUrl` on the next sync.

`events.list` requires a bounded start/end interval and supports bounded
pagination. The interval and `calendarIds` scope are applied before `offset`
and `limit`; `total` is the size of that filtered result. `events.search`
similarly applies its text, date, calendar, account, and invitation-state
filters before pagination.

`events.search` additively accepts `attendee` (not yet released; the protocol
minor remains 2.1): a case-insensitive substring, at most 200 characters, of a
guest's or the organizer's address or display name. When `attendee` is given
the `query` may be empty; a request with neither returns `invalid_params`.

A same-account move preserves the canonical event identity. A cross-account or
cross-provider move durably creates the destination first; deletion of the
source depends on destination acknowledgment, so a destination failure cannot
silently discard the source event.

### Tasks

- `taskLists.list`, `taskLists.setEnabled`
- `tasks.list`, `tasks.create`, `tasks.update`, `tasks.remove`

These methods are additive and not yet released (the protocol minor remains
2.1). Every installation has the device-only list `local-tasks`. CalDAV
collections whose `supported-calendar-component-set` includes `VTODO` appear as
task lists of their account (collections that accept only `VTODO` are no longer
listed as event calendars). Google task lists join them when their sync lands.

- `taskLists.list` returns `lists`: `{id, accountId, name, color, readOnly,
  enabled, position, capabilities, lastSyncAt}`, where `capabilities.provider`
  names the list's provider. `taskLists.setEnabled` takes `listId` and
  `enabled`; disabled lists are left out of the app's "All lists" view.
- `tasks.list` returns `tasks` sorted open first, then by due day (undated
  last). It takes optional `listIds`, `includeCompleted` (default true),
  inclusive `dueStart`/`dueEnd` days (`yyyy-MM-dd`; either one leaves out
  undated tasks), and `offset`/`limit` (at most 2000 per page). Each page
  reports `hasMore` and `nextOffset`; clients read pages until `hasMore` is
  false. A task is `{id, listId, title, notes, dueDate, dueUtc,
  completed, completedAt, priority, parentId, position, dirty, localRevision,
  createdAt, updatedAt}`; `dueDate` is `yyyy-MM-dd` or empty and `dueUtc` holds
  a due time only when the provider keeps one.
- `tasks.create` takes `task` with `title` (required) and optional `listId`
  (default `local-tasks`), `notes`, `dueDate`, `dueUtc`, `completed` and
  `priority` (0–9). `tasks.update` takes `task` with `id` plus the fields to
  change, and optional `expectedLocalRevision`; a task cannot move to another
  list yet. A new `dueDate` without `dueUtc` drops a due time set for the old
  day. `tasks.remove` takes `taskId`. Invalid dates answer
  `invalid_params`; an empty title, a read-only or missing list, or a stale
  revision answers `task_rejected`.
- Completing a task stamps `completedAt`; reopening it clears it. Writes to a
  provider list are saved locally first and marked `dirty` until the provider
  accepts them; the daemon sends them right away and on every sync. CalDAV
  uploads use `If-None-Match: *` for new tasks and `If-Match` for changes and
  removals. When the server copy changed meanwhile, the fields edited locally
  are applied on top of it; other fields, and properties OmaCalendar does not
  edit, keep the server's values.
- Every change broadcasts `tasks.changed` with `listIds` and `revision`
  (topic family `tasks`).

### Conflicts and durable operations

- `conflicts.list`, `conflicts.resolve`
- `operations.list`, `operations.retry`, `operations.discard`
- `outbox.list`, `outbox.retry` are compatibility aliases for diagnostics

Conflict strategies are `keep_remote`, `keep_local`, and `merge`. A merge must
contain a complete, validated editable event draft. Keeping local after remote
deletion recreates the event with a new provider identity.

### Reminders

- `reminders.list`, `reminders.snooze`, `reminders.dismiss`

### Synchronization and ICS

- `sync.all`, `sync.account`, `sync.calendar`, `sync.status`,
  `sync.setInteractive`
- `ics.refresh`, `ics.status`
- `import.preview`, `import.commit`
- `export.create`, `export.run`

`sync.setInteractive` is additive and not yet released (the protocol minor
remains 2.1). A desktop client sends `{"interactive": true}` while its window
is active and `false` when it is not; it returns `interactive`,
`pollIntervalSeconds` and `leaseSeconds`. Google and CalDAV poll every 2
minutes while a client is interactive, every 5 minutes otherwise and every 10
on battery power. "Interactive" lapses after `leaseSeconds` unless renewed, so
a client that exits without saying so cannot keep polling fast. Independently,
the daemon syncs right away (at most once a minute) after resuming from sleep
or when NetworkManager reports full connectivity again.

Import accepts bounded inline content, base64 content, or an absolute regular
local path. Duplicate policies are `skip`, `copy`, and `replace`. Export scope
is one event, a bounded date range, a calendar set, or an entire local
calendar.

### Widget

- `widget.snapshot`

The widget receives a compact presentation-only snapshot and uses revision
subscriptions for missed-change recovery. Recurring snapshot entries include
the authoritative `recurrenceId`; `occurrenceStart` is retained only as a
compatibility display/identity fallback for older widget builds. Widget
mutations send the authoritative value as `eventRef.recurrenceId`. The widget
never opens the database or contacts providers directly.

## Capability negotiation

`system.info` returns protocol/schema versions, the callable method list, and
provider capabilities. App and widget disable unsupported controls and explain
the limitation instead of invoking an unavailable or unsafe operation.

## Stability and deprecation policy

The Quickshell widget is released independently of the app, and IPC 2 is the
natural integration point for third-party Omarchy surfaces — bar modules,
launchers, and scripts. This section states what any of those consumers may
rely on across `omacalendard` versions.

### Compatibility guarantee within a major version

Within `protocolMajor: 2`, the surface described in this document is
additive-only:

- An existing method's accepted params, required fields, and result fields
  never change meaning or get removed in a minor revision.
- A new method, a new optional param, or a new result field ships as a
  `protocolMinor` bump and is discovered through `system.info`'s `methods`
  list — a client must not assume a method is present, only rely on ones it
  found there.
- A renamed method keeps its old name callable as an alias, exactly like the
  existing `accounts.addGoogle`/`google.oauthStart`,
  `accounts.addCalDav`/`accounts.createCalDav`, `outbox.list`/
  `operations.list`, and `outbox.retry`/`operations.retry` pairs documented
  above. A rename is not a removal.

A client that only calls methods it found in `system.info`'s `methods` array,
treats every result field as optional unless this document marks it required,
and rejects (rather than assumes) a `protocolMajor` it doesn't recognize,
stays compatible across every IPC 2.x minor release without a coordinated
update.

### Deprecation notice

Deprecating a method or field lands in the same pull request as the change
that introduces its replacement, per `CONTRIBUTING.md`'s wire-contract rule,
and has two parts:

1. This document notes the deprecation next to the method or field's existing
   entry (for example, "Deprecated: superseded by `X`; see Removal criteria
   below") and adds a row to the Deprecations log at the end of this section
   recording the app version and date the deprecation was announced.
2. The deprecated method or field keeps working exactly as before —
   deprecation is a notice, not a behavior change, and it is not removed from
   `system.info`'s `methods` list during its notice window. `system.info`
   carries no per-method deprecation flag today, so a consumer cannot detect
   a deprecation automatically; it has to track this document.

Minimum notice is one released stable app line (`1.x`, post-`1.0.0`) after
the deprecation is documented here — not the date the replacement shipped,
which may be the same release. Because the widget has its own release
cadence and cannot be forced onto the app's schedule, the window is measured
in stable app releases, so a widget maintainer who updates infrequently still
gets the full window from whenever they next read this document.

### Removal criteria

A deprecated method or field is deleted from the protocol, and from this
document, only when all of the following hold:

- The minimum notice window above has elapsed.
- Nothing else in this document still recommends it to a new consumer.
- The removal itself ships on a `protocolMajor` bump, never a minor one —
  deleting a method a client might still call is a breaking change by
  definition, and a major-version mismatch is already the mechanism this
  protocol uses to reject an incompatible peer cleanly (see Versioning
  above), rather than failing an individual call unpredictably.

In practice, a deprecated IPC 2.x method or field stays callable throughout
2.x and disappears only if and when `protocolMajor: 3` ships. IPC 2 has had
no major bump and no removal to date; the aliases listed above are the only
cases of "old name kept alive alongside a new one" the protocol currently
carries, and none are scheduled for removal.

### Deprecations

None recorded yet.
