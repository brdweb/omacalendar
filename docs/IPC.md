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
