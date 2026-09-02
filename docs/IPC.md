# Local IPC protocol 2.0

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

Every request carries `protocolMajor: 2`. A different major is rejected with
`protocol_mismatch`; minor additions are discovered through `system.info`.

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
`topics` subscribes to `*` for IPC 2.0 compatibility; an explicit list replaces
the connection's prior list and may contain `*`, a family such as `events`, a
family wildcard such as `events.*`, or an exact event such as
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
- `calendars.upsert` for writable device-only calendars
- `calendars.remove` for confirmed custom device-only calendars and owned,
  non-primary Google calendars; built-in, primary, shared, and read-only
  calendars are protected
- `calendarSets.list`, `calendarSets.upsert`, `calendarSets.remove`,
  `calendarSets.activate`
- `settings.get`, `settings.set`

### Events and invitations

- `events.list`, `events.get`, `events.search`
- `events.create`, `events.update`, `events.remove`, `events.move`,
  `events.respond`, `events.undo`
- `invitations.list`, `invitations.markSeen`

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

`events.list` requires a bounded start/end interval and supports bounded
pagination. The interval and `calendarIds` scope are applied before `offset`
and `limit`; `total` is the size of that filtered result. `events.search`
similarly applies its text, date, calendar, account, and invitation-state
filters before pagination.

A same-account move preserves the canonical event identity. A cross-account or
cross-provider move durably creates the destination first; deletion of the
source depends on destination acknowledgment, so a destination failure cannot
silently discard the source event.

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

- `sync.all`, `sync.account`, `sync.calendar`, `sync.status`
- `ics.refresh`, `ics.status`
- `import.preview`, `import.commit`
- `export.create`, `export.run`

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
  `accounts.addCalDav`/`accounts.createCalDav`, and `outbox.list`/
  `outbox.retry` pairs documented above. A rename is not a removal.

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
