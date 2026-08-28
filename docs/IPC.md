# Local IPC protocol

## Transport

The protocol uses UTF-8 JSON objects separated by a single newline over a
user-only local socket at `$XDG_RUNTIME_DIR/omacalendar/daemon.sock`.

Clients and the server enforce a 1 MiB maximum frame size. Calendar event
descriptions or bulk result sets that could exceed this limit are paginated.

## Versioning

The first protocol is `1.0`. Major-version differences are incompatible;
minor additions are capability-gated. Every request includes `protocolMajor`.

## Request

```json
{"id":"8aee...","protocolMajor":1,"method":"system.ping","params":{}}
```

## Success response

```json
{"id":"8aee...","result":{"ok":true,"protocolMajor":1,"protocolMinor":0}}
```

## Error response

```json
{
  "id":"8aee...",
  "error":{
    "code":"invalid_params",
    "message":"start must be an ISO date-time",
    "retryable":false
  }
}
```

Error messages must not contain credentials or raw provider bodies.

## Notification

```json
{"event":"events.changed","data":{"calendarIds":["..."],"revision":42}}
```

Notifications are hints. A reconnecting client always re-queries current state
and never assumes it received every notification.

## Phase 4 checkpoint method surface

### System

- `system.ping`
- `system.info`

### Accounts and authentication

- `accounts.list`
- `accounts.createCalDav`
- `accounts.remove`
- `accounts.test`
- `google.configureClient`
- `google.oauthStart`
- `google.oauthCancel`
- `google.disconnect`

### Calendars and settings

- `calendars.list`
- `calendars.upsert` (development/diagnostic use)
- `settings.get`
- `settings.set`

### Events

- `events.list`
- `events.get`
- `events.create`
- `events.update`
- `events.remove`
- `events.search`

`events.list` always requires a bounded start/end interval and supports a
bounded `limit` plus integer `offset`.

### Synchronization

- `sync.all`
- `sync.account`
- `sync.status`
- `outbox.list`
- `outbox.retry`

## Planned additive surface

The post-checkpoint app/widget work adds these methods without changing
protocol major version 1:

- `system.subscribe`
- `calendarSets.list`, `calendarSets.upsert`, `calendarSets.remove`
- `conflicts.list`
- `conflicts.resolve`

## Capability negotiation

`system.info` returns server, schema, and protocol versions plus method and
provider capabilities. A client disables unavailable UI rather than invoking
unknown methods.
