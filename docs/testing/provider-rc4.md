# RC4 provider acceptance — 2026-09-09

The frozen local **1.0.0-rc.4** daemon and CLI passed **15/15** provider scenarios against disposable Radicale 3.7.8 and authenticated HTTPS ICS. Their SHA-256 values matched before and after the run:

| Executable | SHA-256 |
| --- | --- |
| `omacalendard` | `15c06c654225f2c1e1e502a22b65316a749c65b6c6769d0237bd90c2c2a5d205` |
| `omacalendarctl` | `95ab4bf7000d147432ca8b199a1f58ed261f124398118e63ad62f9694f6ff866` |

Coverage includes remote CRUD, durable offline writes, daemon/server restart, remote deletion, recurrence exceptions and cancellation, all-day exclusive dates, time zones, multiple alarms, disconnect/reconnect/removal, both independently ordered automatic conflict winners, all three manual conflict strategies, and recreation after remote deletion. HTTPS ICS checks cover credentials, conditional refresh, read-only behavior, recurrence inclusions/exclusions, 401/404/503/malformed-response cache retention and authoritative empty-feed deletion. ICS import/export preserves recurrence and alarms across skip/replace/copy and restart.

A fresh CalDAV calendar now qualifies through `calendars.probeThisAndFuture`. The test verifies that future writes are rejected before proof, the explicit check removes its temporary event without changing the user's event or creating a user outbox operation, and a subsequent future edit affects exactly the selected and later occurrences across restart. The editor offers **Check this-and-future support** before enabling that scope. CalDAV future RSVP requires separate scheduling proof and remains disabled; ordinary future edits retain their storage capability.

Five standalone probe fixture cases passed on the final build (**7 QtTest results including setup/teardown**): supported server, stripped RANGE, initial/final cleanup failures with safe retry, and lost create acknowledgement. They also verify busy requests, disconnect and credential replacement cannot interrupt cleanup. Existing mutation-probe regressions passed separately. QML checks verify that requesting a check does not grant scope and that stored RANGE support cannot enable a future RSVP.

The AppController local-date/conversion regressions passed **11/11** results in a minimal `/etc` namespace with UTC `/etc/localtime`, no `TZ`, a private writable `/tmp`, and an empty executable search path. This reproduces the environment where Qt's named system-zone fallback disagreed with its actual local clock; default conversions now use local-time semantics while explicit named zones remain explicit.

## Reproduction and retained evidence

```bash
python3 packaging/release/test-provider-acceptance.py \
  --daemon artifacts-rc4-release/local-bin/omacalendard \
  --cli artifacts-rc4-release/local-bin/omacalendarctl \
  --radicale /tmp/omacalendar-provider-runtime.WD12qv/venv/bin/radicale \
  --manual-conflicts --output artifacts-rc4-release/provider-final-isolated
```

The Radicale path is the disposable test environment used for this run; substitute a Radicale 3.7.8 executable when reproducing. Locally retained evidence is under `artifacts-rc4-release/`: `provider-final-isolated.log`, its `results.json` and `identity.json`, `probe-fixture-final.log`, and `appcontroller-minimal-etc-isolated.log`. The earlier `provider-final` receipt is retained: fresh qualification passed, but the follow-up reused that newly successful series UID and hit a harness indexing error. Distinct fixture identities corrected the test; the binaries were unchanged.

Only synthetic accounts, credentials and events were used. All test-owned processes stopped. This report does not qualify real Google/Fastmail accounts, desktop credential storage, provider invitations, or downloaded release packages. Those acceptance results and immutable RC3 receipts remain separate.
