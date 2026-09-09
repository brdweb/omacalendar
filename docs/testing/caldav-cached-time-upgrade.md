# Cached CalDAV time metadata upgrade

RC3 could store floating CalDAV events as `zoned`. A normal subsequent sync could
retain that classification indefinitely when the server's ctag/etag was unchanged.
This affected cached profiles even after the parser and floating wire conversion
were corrected.

The fixed daemon performs a versioned local metadata refresh from its retained
lossless iCalendar resources before restoring CalDAV accounts or replaying queued
writes. It does not need network access or a changed provider revision. It also
covers disabled accounts and cached resources outside the current fetch window.

The database savepoint updates only the affected `time_kind`/`timeKind` metadata,
rebuilds derived occurrence/reminder data, and records
`provider_state.caldav_time_kind_version=1` for the account in the same commit.
Canonical event IDs, recurrence references, event fields, edit timestamps, local
revisions, provider validators, preferences, mutation identifiers, retry states and
queued changes are retained. Active outbox snapshots and unresolved conflict
snapshots receive the same metadata correction, including retained move sources.
An explicitly populated timezone is preserved as an intentional local choice.

A missing or invalid required retained resource fails the refresh without a
checkpoint or partial changes. The next attempt retries from the previous cache;
the daemon reports `cache_metadata_refresh_failed` instead of replaying an
unclassified write. The refresh does not infer corrections from current server
content or discard an unsent edit. This is a metadata refresh, not a database schema
reset or a reauthorization flow.

## Reproducible validation

Run the database suite and the real provider runner with an immutable RC3 daemon
as `--previous-daemon` and the proposed daemon as `--daemon`:

```sh
ctest --test-dir build-stable -R '^database_test$' --output-on-failure
python3 packaging/release/test-floating-caldav.py \
  --daemon build-stable/omacalendard \
  --cli build-stable/omacalendarctl \
  --previous-daemon artifacts-1.0.0-rc.3/verification/native/usr/bin/omacalendard \
  --radicale /path/to/disposable/venv/bin/radicale \
  --work artifacts-acceptance-2026-09-09/work/desktop-services \
  --report artifacts-acceptance-2026-09-09/reports/floating-upgrade-fixed.json
```

The runner creates a private profile, synthetic credentials and a real loopback
Radicale server. RC3 populates a recurring floating master and existing detached
exception, then queues an offline local edit. A synthetic database trigger rejects
the checkpoint to exercise rollback. After removing that failure, the upgrade runs
offline, checks every canonical row and queued-operation field, restarts, and then
replays the old edit to Radicale. Raw provider readback must retain floating 09:00
wall times across the New York DST boundary. The runner also retains the existing
create, occurrence patch, original reference reuse, acknowledgment and restart
checks. It cleans its isolated profile and server afterward.

The database regression additionally injects a parsing interruption after a staged
row update and verifies checkpoint failure, retry after reopen, intentional zoned
metadata, unchanged local edits and operation timestamps, unresolved conflict
snapshots, and version idempotence.

## Qualification evidence (2026-09-09)

- Immutable RC3 fails the new offline upgrade regression: cached kinds remain
  `zoned`. Receipt: `artifacts-acceptance-2026-09-09/reports/floating-upgrade-rc3-regression.json`.
- Proposed source build and final test receipts are recorded separately from RC3;
  this document does not relabel the immutable RC3 payload as fixed.

- Final frozen RC4 daemon
  `15c06c654225f2c1e1e502a22b65316a749c65b6c6769d0237bd90c2c2a5d205`
  passes the real RC3 upgrade and all nine floating provider checks. Receipt:
  `artifacts-acceptance-2026-09-09/reports/floating-upgrade-final-rc4.json`.
- The full database suite passed initially (2.55 seconds) and parent-run combined
  validation passed again with the final conflict snapshot assertions.

## Related local-time acceptance

The same frozen RC4 daemon passes two additional isolated regressions found during
functional review. In a private bubblewrap filesystem with empty `/etc` and `TZ`
unset, an all-day reminder now lands exactly at midnight; the earlier build fired
one second early. In `America/New_York`, narrow UTC queries spanning the 2030
fall-back hour and the spring clock gap return the same occurrence as a broad
query. The earlier floating iterator window incorrectly omitted both occurrences.
The gap check derives its window from the canonical instant returned by the broad
query, allowing Qt's local-time normalization of a nonexistent wall time.

Receipts in `artifacts-acceptance-2026-09-09/reports/`:

| Check | Before | Final RC4 |
| --- | --- | --- |
| Empty `/etc`, all-day reminder | `all-day-reminder-minimal-before.json`: FAIL | `all-day-reminder-minimal-final-rc4.json`: PASS |
| Floating fall-back narrow query | `floating-narrow-fold-before.json`: missing occurrence | `floating-narrow-fold-final-rc4.json`: PASS |
| Floating spring-gap narrow query | `floating-narrow-gap-before.json`: FAIL | `floating-narrow-gap-final-rc4.json`: PASS |

The reproduction scripts are retained in the isolated acceptance work directory
under `work/desktop-services/review-*.py`. No personal calendar, account, keyring or
active desktop service was used. These checks qualify the identified local RC4
snapshot; exported release packages require their own immutable qualification.

Parent-run combined validation of this snapshot also passed all 20 functional
CTest suites (34.51 seconds), qmllint, all 24 recurrence QtTest cases in the
minimal `/etc` environment (43 milliseconds), and the floating wire-identity
round-trip case. The frozen RC4 desktop passed all 16 AT-SPI checks at 125% scale;
receipt: `artifacts-rc4-release/desktop-local-system-python/result.json`.
