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

## Provider matrix

Before a public release, run the Google test matrix's CRUD, all-day,
recurrence, exception, offline, restart, and disconnect cases against:

- Radicale on localhost;
- Nextcloud; and
- at least one hosted CalDAV provider.

Also verify a read-only calendar rejects local editing, a stale ETag produces a
visible blocked/conflict state, invalid credentials result in
`reauthorization_required`, and a server without sync tokens removes stale
cached resources after a successful full rebuild.
