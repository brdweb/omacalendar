# Design note: tasks and the schema-2 decision

## Status

Design record only. No implementation, no new tables, no new IPC methods.
Tasks remain out of 1.0 scope per
[PLAN.md](PLAN.md#excluded-from-10); this note exists so the schema
question is answered while schema 2 is still open to change, before it
freezes for a stable release.

## Question

CalDAV collections OmaCalendar already discovers can also host `VTODO`
resources (nullable `DTSTART`, optional `DUE`, `STATUS`,
`PERCENT-COMPLETE`, `COMPLETED`). Does the schema 2 event model absorb
`VTODO`, or does a tasks feature require a schema 3 migration?

## Findings

### The `events` table is already loosely typed, but the write path is not

`events.start_utc`, `end_utc`, `start_date`, `end_date` are all
`TEXT NOT NULL DEFAULT ''` (`database.cpp:482-485`) — SQLite itself already
tolerates an empty/absent value on every date column; there is no `CHECK`
forcing a start to exist. The actual gate is `Daemon::validateEventTimes`
(`daemon.cpp:90-104`), which unconditionally requires a valid start and end
for both all-day and timed events. A dateless `VTODO` (no `DTSTART`, no
`DUE`) would be rejected by this validator today, regardless of which table
it lands in — a tasks feature needs a task-aware validator either way, so
this is not a factor in the schema choice.

### The recurrence expander already tolerates a missing start

`RecurrenceExpander::overlaps()` (`recurrenceexpander.cpp:54-72`) checks
`event.startUtc.isValid()` / `event.startDate.isValid()` first and returns
`false` — excluded from the window — rather than crashing, if a start is
absent. A dateless task stored in `events` would simply never appear in a
bounded `events.list`/widget-snapshot range query, which is actually the
*correct* behavior for a task with no due date — but it means a tasks
feature needs its own query surface (a task list, not a date-range query)
regardless of which table holds the row. One caveat: a *recurring* `VTODO`
still needs `DTSTART` or `DUE` as an anchor for `RRULE` expansion per RFC
5545 §3.6.2, via the same `icalcomponent_foreach_recurrence` libical call
the event expander already uses (`recurrenceexpander.cpp:380-409`). So
"no start at all" is only a real scenario for *non-recurring* tasks — the
common case, but worth stating precisely.

### CalDAV sync does not currently discover `VTODO` — it actively excludes it

The roadmap's framing ("CalDAV servers expose `VTODO` on collections
OmaCalendar already discovers") is true only at the collection level. The
bounded fetch REPORT hard-filters to events:
`<c:comp-filter name="VEVENT">` (`caldavclient.cpp:107-113`). The
incremental `sync-collection` REPORT has no component filter at all
(`caldavclient.cpp:116-127`), so a `VTODO`-only resource could slip through
an incremental sync today — and would then hit
`icalcodec.cpp:1031`'s `"The calendar resource contains no VEVENT"` parse
error, not a silent skip. Collection discovery (`PROPFIND`,
`caldavclient.cpp:90-99`) also never requests
`supported-calendar-component-set`, so OmaCalendar has no way to know today
whether a discovered collection even supports tasks. All of this is
sync-side work a tasks feature needs regardless of the schema answer — it
is not "discovered but skipped," it is entirely unbuilt.

### Google Tasks is a separate API, not an extension of Google Calendar

No `VTODO`/Tasks code exists under `src/providers/google/`, and there is
none to find: Google Calendar's API does not expose tasks — Google Tasks is
a distinct API and product. A first tasks release scoped to CalDAV only
(the roadmap's framing) is therefore not just the *simplest* starting
point, it is the only one that reuses the existing provider shape at all;
Google task support would be a second, later, differently-shaped provider
integration, not a VTODO addition to `GoogleSync`.

### The project already has precedent for additive, non-versioned schema repairs

`Database::ensureSyncCoverageSchema` and the reminder-delivery repair
function add new tables/columns via `CREATE TABLE IF NOT EXISTS` /
`ALTER TABLE ADD COLUMN` at daemon startup, outside the versioned
`schemaVersion()` migration path (`database.cpp:1093-1109`,
`database.cpp:1030-1090`). The comment on the sync-coverage repair is
explicit about why this is safe: *"Schema 2 is still the development
schema. Keep this repair idempotent so databases created by earlier
development builds gain durable coverage without being reset."* That
reasoning is scoped to pre-1.0 development churn, not a general license —
once schema 2 is the frozen, released schema
(`COMPATIBILITY.md`'s `1.0.x | 2 | 2` row), an unannounced DDL change would
violate the exact-version compatibility promise that row makes.

The separate migration mechanism — `schemaVersion() == 1` triggers
archive-the-old-file, stage-a-fresh-database, require-reconnection
(`database.cpp:185-210`) — is a full reset with a timestamped backup, not
an in-place `ALTER`. It is deliberately blunt: safe, but it forces every
account to reconnect and every local cache to rebuild. That cost is what
"schema 3" actually means in this codebase, and it is the same cost
regardless of how small the schema 3 diff is.

## Candidate shapes

**A. Fold `VTODO` into `events`.** Add nullable `due_utc`/`due_date`,
`percent_complete`, `completed_at` columns and a `kind` discriminator
(`event`/`task`) to the existing table.

- Cost: every one of the roughly two dozen columns that make no sense for a
  task (`organizer_json`, `attendees_json`, `conference_url`,
  `transparency`, `visibility`, ...) stays on every task row. Every
  consumer of `events` — the recurrence expander, `events.list`/search,
  the widget snapshot, conflict resolution, the outbox — would need to
  either learn to filter `kind`, or risk a task leaking into a
  calendar-grid code path that was never written to expect one, since
  `events.list` and `widget.snapshot` are both range queries against this
  one table today. The blast radius is the entire existing event surface,
  for a feature that is explicitly excluded from 1.0.
- Benefit: reuses the existing outbox/conflict/sync-coordinator machinery
  without writing a second copy of it.

**B. A sibling `tasks` table**, referencing `calendars`/`accounts` exactly
as `events` does, with its own `due_utc`/`due_date`, `status`,
`percent_complete`, `completed_at`, and the same durability columns
`events` already carries (`dirty`, `deleted`, `local_revision`,
`sync_state`, `raw_payload`, `created_at`, `updated_at`) so the outbox and
conflict-resolution *shapes* generalize even if their code doesn't
initially.

- Cost: a second table, a second domain struct, a second IPC surface
  (`tasks.list`, etc.), a second presentation DTO. More net-new code than
  option A.
- Benefit: `events` is untouched — zero risk to the recurrence expander,
  widget snapshot, search, or any existing IPC contract. It composes with
  the existing `RecurrenceExpander` for the rare recurring-task case
  (the expander operates on any RFC-5545 component with a `DTSTART`/`RRULE`
  shape; a `Task` struct feeding the same libical call is a plausible reuse,
  not a redesign). It would want its own FTS table alongside `events_fts`
  (`database.cpp:730-733`) for the same reason events have one.

## Recommendation

**Option B — a sibling `tasks` table — and it does not need to be schema
3.**

A purely additive table (no `ALTER` to `events`, `calendars`, or any
existing table; no column removed or repurposed) is exactly the shape the
project already treats as a safe, idempotent startup repair
(`ensureSyncCoverageSchema` is the precedent) and is the same *additive
within a major version* philosophy the IPC 2 stability policy just
formalized in `docs/IPC.md` (new methods/fields are a minor addition,
discovered rather than assumed; nothing existing is ever removed or
repurposed without a major bump). An old client that has never heard of
`tasks` simply never queries it — nothing about the existing schema-2
contract in `COMPATIBILITY.md` changes for it, the same way an unopened
comment thread doesn't change compatibility for a client that never
subscribes to it.

This directly answers the roadmap's real question: **the first tasks
release does not have to spend a migration.** No archive-and-reconnect, no
forced reconnection, no local-cache rebuild — the `tasks` table is created
the same way `sync_coverage` was, at daemon startup, the first time a build
that knows about it runs.

The one thing this recommendation does *not* settle, and should be decided
explicitly (not by default) whenever tasks are actually scheduled: whether
adding a table this size still counts as "schema 2" in the
`COMPATIBILITY.md` sense once 1.0 is stable and that row is no longer a
development convenience — i.e., whether the exact-version compatibility
promise is read as "the DDL is byte-identical" or "existing tables/columns
are byte-identical, additions are fine," matching how `protocolMinor` on
the IPC side already draws that line. This note recommends the latter
reading, consistent with the IPC precedent, but it's a policy call for
whoever schedules the tasks work to make on purpose rather than by silent
precedent.

## Explicitly out of scope for this note

- Capability discovery for "does this calendar support tasks" (a
  `supported-calendar-component-set` PROPFIND addition and a
  `system.info` capability flag) — sync-side work, not schema.
- Whether the outbox/conflict-resolution *code* generalizes to tasks or is
  duplicated — an implementation decision for whoever builds it.
- Google Tasks — a distinct, later provider integration.
- Any IPC method shape (`tasks.list`, `tasks.complete`, ...).
