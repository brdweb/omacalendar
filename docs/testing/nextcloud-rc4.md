# Nextcloud RC4 acceptance — 2026-09-09

**PASS: baseline 8/8 and expanded 10/10 checks.** This receipt records a local RC4 build; published package identity remains a separate release check.

## Candidate and server

- App version: `1.0.0-rc.4`, verified through the isolated daemon IPC endpoint.
- Daemon SHA256: `15c06c654225f2c1e1e502a22b65316a749c65b6c6769d0237bd90c2c2a5d205`.
- CLI SHA256: `95ab4bf7000d147432ca8b199a1f58ed261f124398118e63ad62f9694f6ff866`.
- Nextcloud: **34.0.3**, official Apache image `nextcloud@sha256:b97df9e0e1ee3c8c6cc009cb3f12ddce915d624d543b3bb93882025fe323a407`.
- Disposable server: SQLite, 768 MiB RAM, one CPU, dedicated network, loopback-only HTTP port. Two synthetic local users with `.test` addresses; private app profiles and synthetic Secret Service.
- Server email invitations, external attendees and event reminder emails disabled. Local CalDAV scheduling remains enabled; no real account or external invitation was used.

## Results

| Matrix | Result | Covered behavior |
| --- | --- | --- |
| Baseline | **8/8 PASS**, 37.171 s | Discovery; timed CRUD/readback, timezone/two alarms; exclusive multi-day dates; recurrence and detached update; cached offline access/durable queue; server/daemon restart; local/remote deletion; disconnect/removal |
| Extended | **10/10 PASS**, 81.064 s | Read-only shared ACLs; first-use RANGE qualification and probe cleanup; local guest scheduling; accepted/tentative/declined RSVP and pending-list removal; recurring occurrence RSVP; safe future-RSVP rejection with whole-series fallback; keep-remote/keep-local/merge conflicts; remote-deletion recovery |

Both final runs completed without server worker crashes. All disposable containers, volumes and networks were removed, independently checked after completion. Both binary hashes still matched afterward.

An early baseline run reused an event revision after independent remote queries overlapped canonical synchronization, and correctly received a stale-revision rejection. The harness now opens the current revision immediately before updating. That failed run is preserved separately; no runtime change was needed for it.

## Future RSVP is intentionally unavailable

Nextcloud's successful ordinary `RANGE=THISANDFUTURE` storage does **not** establish scheduling propagation. A real two-user counterexample produced five recurring instances: three were accepted in the attendee calendar, but only one was accepted in the organizer calendar after both daemons restarted. The attendee resource retained RANGE; Nextcloud's organizer reply stripped it.

RC4 therefore publishes `rsvpThisAndFuture=false` independently of storage qualification. The final negative check verified rejection without changing the local event or either remote calendar and without creating durable work; it then successfully accepted the same entire series. A separate occurrence reply updated exactly one of five instances in both calendars. Occurrence and whole-series replies remain supported. Ordinary future event edits remain available after successful collection-specific qualification.

## Reproduction and evidence

From the repository root, run `packaging/release/test-nextcloud-acceptance.py` with `--bin artifacts-rc4-release/local-bin --version 1.0.0-rc.4`, the pinned `--image` above, a disk-backed `--work`, and a new `--output` directory. Run once for the eight baseline checks and again with `--extended-only` for ten expanded checks. This requires existing Docker and the app's native dependencies; it does not install host packages or alter the desktop.

Detailed local receipts: `artifacts-rc4-release/verification/nextcloud-baseline-final/` and `nextcloud-extended-10-final/`. Earlier diagnoses are retained in `artifacts-acceptance-2026-09-09/logs/nextcloud/`; the detailed future-RSVP counterexample is `scoped-rsvp-c216-detail/scoped-rsvp-future.json`.

These checks establish the stated real Nextcloud behaviors on local HTTP/SQLite with synthetic credentials. They do not establish hosted-provider or production-TLS behavior, outbound mail, real desktop keyring, GUI interaction, native/Flatpak installation, clean-Omarchy acceptance, or untested calendar-move/credential-rotation cases. Fastmail and Google remain separate provider gates.
