# Architecture

## Process boundaries

### `omacalendard`

The daemon exclusively owns:

- the writable SQLite connection;
- provider network access;
- account authentication state;
- Secret Service access;
- recurrence expansion and occurrence caches;
- the mutation outbox and conflict records;
- reminder scheduling; and
- the local IPC server.

Only one daemon instance may own a data directory. SQLite and a user-only local
socket provide the enforcement points.

Packaged installations use a systemd user socket as the stable IPC endpoint.
The socket exists independently of either client and starts `omacalendard` on
the first connection. The daemon opens the local database and answers cached
snapshot queries immediately; provider synchronization remains asynchronous.
Consequently, the shell widget can display calendar data while the desktop
application is closed.

### `omacalendar`

The desktop application owns presentation and user interaction. It reads and
writes through IPC and may keep disposable in-memory view models. It does not
open the calendar database or read credentials.

The application is the canonical settings UI for both itself and the shell
widget.

### Omarchy widget

The QML plugin is a thin presentation client. It is deliberately unable to access
remote provider credentials. Shell-only state such as bar placement remains in
Omarchy's `shell.json`; calendar and widget behavior is configured through the
desktop app and daemon.

Both clients consume presentation DTOs and revisioned snapshots. Provider URLs,
ETags, tokens, raw resources, and mutation payloads remain daemon-private. After
a reconnect, a client compares its last observed revision with the daemon and
refreshes the relevant snapshot when notifications may have been missed.

## Storage locations

The daemon follows XDG directories:

```text
$XDG_DATA_HOME/omacalendar/calendar.sqlite3
$XDG_CONFIG_HOME/omacalendar/config.json
$XDG_CACHE_HOME/omacalendar/
$XDG_RUNTIME_DIR/omacalendar/daemon.sock
```

Defaults are used when an XDG variable is absent. Credential values live in
the Secret Service collection and are referenced by account UUID.

## Domain model

### Account

An account identifies a provider connection and contains only non-secret
configuration: provider kind, display name, principal identifier, endpoint,
enabled state, and last authentication status.

### Calendar

A calendar belongs to one account. It retains a stable local UUID and remote
identifier/href, display metadata, permissions, provider capabilities, and
incremental sync state.

### Event

An event stores a stable local UUID, calendar association, remote identifier,
iCalendar UID, revision identifier, user-visible fields, original time zone
identifiers, recurrence information, attendee/reminder JSON, and a provider raw
payload for lossless fields. Provider raw payloads are never rendered directly.

All-day event boundaries are ISO dates. Timed boundaries are UTC instants plus
the original IANA time zone identifiers used for display and round trips.

### Outbox operation

Every remote-provider mutation is committed atomically with an outbox operation.
Device-only mutations complete in the local transaction. A remote operation
stores a stable client mutation ID, expected local and remote revisions,
dependency and recurrence scope, guest-notification policy, retry/lease state,
and redacted error metadata.

### Conflict

A conflict briefly records the local entity and both expected/observed remote
revisions. The remote version remains separate until background last-write-wins
resolution commits one version without exposing a blocking UI workflow.

## Synchronization algorithm

1. Pull remote incremental changes for a calendar.
2. Apply clean remote changes transactionally.
3. Detect remote revisions that conflict with pending local mutations.
4. Compare the local edit timestamp with the provider update timestamp and
   automatically keep the newer version. Ties and missing timestamps prefer
   the provider copy; a remote deletion is dated when it is observed.
5. Dispatch ready outbox operations in stable order.
6. Persist provider acknowledgement and finish the operation in one
   transaction.
7. Advance the monotonic change revision and broadcast compact entity-change
   notifications to connected clients.

If the process loses connectivity after the remote service accepted a write,
the next attempt first resolves the remote UID/revision before repeating a
create. Provider-specific idempotency mechanisms are used where available.

## Provider interface

Google, CalDAV, ICS subscriptions, and device-only calendars implement one
daemon-facing `Provider` contract and are registered with one
`SyncCoordinator`. The contract provides lifecycle, synchronization, status,
capability, and change-notification surfaces. Remote adapters additionally
implement the same conceptual operations:

- authenticate or validate credentials;
- discover calendars and capabilities;
- perform initial and incremental calendar sync;
- fetch an entity by remote identifier;
- create, replace/update, and delete an event; and
- classify errors into authentication, permission, conflict, retryable,
  throttled, unsupported, or terminal categories.

Provider network DTOs never cross IPC. Mapping between those DTOs and the
canonical domain is isolated in each adapter and covered by contract/fixture
tests. The coordinator routes accounts by provider kind; IPC handlers do not
select network clients or inspect provider-owned metadata.

## Threading

The daemon's QObject graph and SQLite connection run on the main daemon thread
initially. Network operations are asynchronous. CPU-heavy recurrence expansion
may use bounded worker tasks, but database access is returned to its owning
thread. No nested event loops are used in provider production code.

## Theme integration

The desktop app watches the active Omarchy theme files and maps foundational
palette, type, spacing, radius, and control-state values into QML properties.
Unknown or malformed tokens fall back independently so a partial theme cannot
make the app unusable.

The widget consumes Omarchy's live QML theme singletons directly rather
than re-parsing theme files.
