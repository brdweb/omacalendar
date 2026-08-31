# CalDAV testing guide

OmaCalendar accepts a CalDAV service URL, username, and password or app
password in **Accounts & settings**. Passwords are sent directly to the local
daemon over its user-only socket and stored in the desktop Secret Service; they
are not stored in SQLite.

## Connection

1. Prefer the provider's documented CalDAV discovery URL. HTTPS is mandatory
   except for loopback-only development servers.
2. Use an app password when the provider supports one.
3. Enter an optional label, endpoint, username, and password, then select
   **Connect CalDAV**.
4. Confirm the account reaches `connected` and its writable and read-only
   calendar collections appear correctly.

Discovery follows `current-user-principal`, `calendar-home-set`, and calendar
collection properties. Synchronization uses `sync-collection` when the server
advertises a token, with a bounded `calendar-query` rebuild for servers that do
not support incremental sync.

## This-and-future capability qualification

CalDAV has no interoperable discovery property proving that a server will
retain RFC 5545 `RANGE=THISANDFUTURE` writes. OmaCalendar therefore keeps the
calendar's `thisAndFuture` capability disabled until it has evidence from that
specific collection.

On the first durable this-and-future mutation for an unqualified writable
calendar, the daemon runs a disposable probe before touching the user's event:

1. Derive a stable probe UID and hidden resource name from the account,
   calendar, and durable client-mutation identity.
2. Delete that exact URL first, accepting not-found, so a retry after a crash or
   lost acknowledgment cannot accumulate probe resources.
3. Create a cancelled, transparent, far-past two-occurrence series; read it
   back; update its exception with `RANGE=THISANDFUTURE`; and read it back again.
4. Require the parsed readback to retain the range parameter, then delete the
   exact probe resource.
5. Persist proof only after successful verification and cleanup, and then replay
   the original durable mutation. The real mutation also receives a readback
   check before local acknowledgment.

Any create/read/update/readback/cleanup failure leaves the capability disabled
and the user's mutation visibly blocked. Deterministic fake-server tests cover
successful proof, stripped range data, failed cleanup, stale-resource recovery,
and a lost create acknowledgment. These tests do not replace live qualification
against Radicale, Nextcloud, or Fastmail.

## Provider matrix

Before a public release, run the Google test matrix's CRUD, all-day,
recurrence, exception, offline, restart, and disconnect cases against:

- Radicale on localhost;
- Nextcloud; and
- Fastmail as the hosted CalDAV provider.

Also verify a read-only calendar rejects local editing, a stale ETag produces a
visible blocked/conflict state, invalid credentials result in
`reauthorization_required`, and a server without sync tokens removes stale
cached resources after a successful full rebuild.

Current development evidence is intentionally partial. An isolated Radicale
3.7.8 instance has passed timed, all-day, multi-day, and recurring creation;
local and remote update/delete; delayed deletion after the undo window; offline
drain across server and daemon restart; remote pull/deletion; a stale-revision
conflict with keep-remote resolution; detached-occurrence update; multiple
alarms; and on-demand coverage hydration. Live guest/RSVP behavior where the
server advertises scheduling, keep-local/merge conflict resolution, disconnect,
and the rest of the complete matrix remain open. The live Radicale slice did
not exercise or qualify this-and-future support. Nextcloud and Fastmail live
acceptance have not yet run.
