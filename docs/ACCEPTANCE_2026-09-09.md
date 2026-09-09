# Agent-executed acceptance — 2026-09-09

**Recommendation: do not publish or promote the RC3 app candidate.** Expanded
testing found reproducible data-correctness and installed-desktop failures
despite the earlier successful build, packaging and smoke checks. Local source
fixes have passed the combined functional suite. A new candidate
must be built and qualified; successful development tests cannot qualify the
unchanged signed RC3 payloads.

This report supplements [OWNER_TESTING.md](OWNER_TESTING.md). It records what
the agent could perform with disposable accounts, servers and profiles. It
does not mark all owner-checklist rows passed or authorize publication.

## Identity and scope

- Baseline/tag target: `a3634a9baf9a74052baaa62baf29cec6f7fb841b`,
  `v1.0.0-rc.3`. The checkout was clean before this work.
- Original downloaded RC3 daemon SHA256:
  `318f1715b40a72a5bd68615f036169ff451b765342be24795d77ab8153c5ffb6`.
- Original CLI SHA256:
  `f8a7793be03552583aa7a5647170e6b16a3aa104cfba1ce35ec813ceed7da653`.
- Runtime: Omarchy/Arch, Linux 7.1.9, Qt 6.11.2, libical 4.0.5,
  Python 3.14.7, America/New_York. Full environment and executable identities
  are under `artifacts-acceptance-2026-09-09/environment`.
- Live hosted status was refreshed read-only: RC3 remains a draft and the
  public release remains beta. The exact-tag package workflow
  [34306444464](https://github.com/brdweb/omacalendar/actions/runs/34306444464)
  and merged-tree CI
  [34306392758](https://github.com/brdweb/omacalendar/actions/runs/34306392758)
  were successful. These are earlier package/CI evidence, not new full
  desktop/provider acceptance.
- Tests used extracted candidate executables, disposable HOME/XDG roots,
  private D-Bus/keyrings/displays, real loopback Radicale and Nextcloud, and
  process-owned HTTPS ICS. No personal calendar, credential store or active
  desktop service was changed. No release, commit or push was performed.

Detailed local evidence is retained under
[`artifacts-acceptance-2026-09-09`](../artifacts-acceptance-2026-09-09/).
That directory is ignored by Git; this summary and the reusable harnesses
are workspace source files and must accompany any eventual review.

## Completed testing

| Area | Result and practical limit |
| --- | --- |
| Original artifacts | All 19 entries in the downloaded RC3 checksum manifest passed again. Candidate signature/attestation and hosted identities were reconciled. Original daemon/CLI contract passed before adding the newly discovered regressions. |
| Beta upgrade and recovery | Expanded exact published-beta to exact-RC3 preservation passed: full synthetic event DTOs, recurrence exceptions/cancellation, guests/organizer metadata, alarms, nine settings, calendar preferences/sets/defaults, accounts, two restarts and older-backup restore. Legacy schema-1 WAL/SHM archive, private permissions, reconnect and obstructed-migration safe failure/retry passed. |
| Native desktop services | 12 exact-RC3 scenario groups passed using real private gnome-keyring/libsecret, both keyring and daemon restart with new remote data, credential rotation/disconnect/removal, namespace separation, notification D-Bus actions/privacy/retry and inherited listening socket handling. This is not physical notification, locked-user-keyring or host-systemd acceptance. |
| Installed GUI | Original RC3 fails native date selection and accessible search naming. Corrected development app passes 16 assertions at each of 100%, 125% and 200% on private Xvfb: toolbar creation from all five views, precise times, real pointer drag/resize, Ctrl+Z, Ctrl+F, Escape, search result activation, deep-link single-instance editing, all-day dates, delete and restart. Physical Wayland/mixed-monitor behavior remains pending. |
| Radicale and HTTPS ICS | Original RC3: 8 passing and 3 failing scenario groups. Final frozen combined build: 14 passing and 1 remaining failing group (fresh-calendar this-and-future). Real transport covers both conflict orderings and all three manual choices, recurrence exceptions/cancellation, alarms/zones, offline/restart/deletion/account lifecycle, HTTPS authentication/error/cache behavior and ICS import/export duplicate policies. |
| Nextcloud | Official digest-pinned Nextcloud 34.0.3 independently reproduces RC3's all-day acknowledgement failure. Final frozen combined build passes all eight scoped checks in 36.479 seconds, including timed/all-day writes, recurrence/detached updates, restart, offline exactly-once replay, deletion and disconnect/removal. No external hosted account, scheduling or full Nextcloud matrix claim is made. |
| Recurrence regressions | Exact RC3 fails partial-occurrence date preservation and floating-occurrence duplicate prevention. Local fixes pass UTC, spring/fall DST, floating and multiday all-day cases, repeated detached edits, resize, invalid-reference rejection and restart. |
| Combined source validation | Full build, warning-free QML lint and 20/20 functional CTest suites passed (16.99 seconds), including all newly added unit/daemon regressions. Both performance CTest suites also passed, for 22/22 registered suites overall. C++ formatting, Python compilation and Git whitespace checks passed. |
| 100,000-event performance | Final development daemon passed enforced p95 gates: agenda 39.307 ms / 200 ms limit, search 4.754 ms / 250 ms, widget snapshot 65.918 ms / 100 ms. The measured hardware run used five warmups and 15 samples after fixture servers and UI tests stopped. This measures daemon IPC/database paths, not physical desktop gesture latency. |

Detailed reports:
[upgrade](../artifacts-acceptance-2026-09-09/reports/upgrade.md),
[desktop services](../artifacts-acceptance-2026-09-09/reports/desktop-services.md),
[installed GUI](../artifacts-acceptance-2026-09-09/reports/installed-ui.md),
[providers](../artifacts-acceptance-2026-09-09/reports/providers.md),
[Nextcloud](../artifacts-acceptance-2026-09-09/reports/nextcloud.md), and
[recurrence](../artifacts-acceptance-2026-09-09/reports/recurrence-fix.md).
Their pass counts overlap and must not be summed into a single test total.

The final combined development snapshot is frozen in
`artifacts-acceptance-2026-09-09/work/final-combined-bin`. Its daemon SHA256 is
`b4191bd0898acd767c5c4a602ea4eb9d1fd16a69232315839213d1af542cd932`;
app SHA256 is
`a898dd7bc03f2486506597542f9586f0b1098468b5c96434e1a9803abfd9d9f0`.
This is modified local source retaining the RC3 version string; hashes, not
that version string, distinguish it from the immutable candidate. No fixes
were substituted into the draft assets.

After the final daemon changes, all 16 installed-UI assertions passed again
at 125% using that frozen combined snapshot
(`work/ui-final-combined/result.json`). The app executable itself is identical
to the earlier successful 100%/125%/200% runs. Final transport evidence is in
`work/providers/final-combined/results.json` and
`logs/nextcloud/final-combined/results.json`. Test server containers, volumes,
networks, application/display/keyring processes and transient profiles were
cleaned up. The inert Nextcloud image and extracted Xvfb tool remain cached.

Combined check logs are `logs/final-build.log`, `logs/final-qmllint.log`,
`logs/final-ctest-functional.log`, `logs/final-ctest-performance.log` and their
saved detailed CTest logs. `environment/source-manifest.json` records the
modified/untracked source file hashes; `environment/source.patch` records the
tracked-file diff. The original extracted RC3 binary hashes were checked
again at completion and remain unchanged. Final source changes are local and
uncommitted.

## Findings and local changes

| Finding | Observed impact | Current disposition |
| --- | --- | --- |
| Partial occurrence patch uses master dates | Renaming the second recurring instance silently moves it to the first instance's date; persists after restart. | Local daemon fix resolves the selected occurrence before merging a partial patch. Regression passes. Previously misdated data is not automatically repaired. |
| Floating occurrence identity mismatch | Editing one of three floating occurrences leaves four visible instances. | Local recurrence matching preserves existing emitted IDs while replacing the intended instance. Regression passes. |
| Conflict ordering invents remote edit time | An older remote edit can silently replace a newer local edit. Sync bookkeeping can also manufacture a new local timestamp. | Local codec/database fixes read revision timestamps, preserve real edit chronology, and leave missing/tied times unresolved. Unit regressions pass; real transport checks prove both known edit orderings and manual choices. |
| All-day/floating CalDAV identity is lost | All-day writes reach both Radicale and Nextcloud but remain pending/sending locally. | Local parser/serializer preserves time kind; real all-day acknowledgement and restart pass on both servers. Floating recurring wire/reference matching passes real Radicale readback, clean acknowledgement, repeated edits and restart. |
| Native date crosses the UTC boundary | Day view uses the previous date in New York/Los Angeles and omits the selected day's events. | Local QDateTime boundary fix passes real native-to-JavaScript regressions in five zones and installed UI workflows. |
| Unnamed event rows | Screen-reader/accessibility clients cannot identify rendered Agenda/search results by title. | Local explicit accessible name/description/role passes real AT-SPI naming and activation. |
| Fresh CalDAV this-and-future probe is unreachable | UI/API reject the first mutation before the documented capability probe can run, even on independently verified capable Radicale. | **Unresolved.** External RANGE object seeding proves the server supports it and subsequent app mutations work; this cannot be marked unsupported/N/A. Requires an explicit capability-probe entry path and its UI/API tests. |
| Existing RC3 floating CalDAV cache is not normalized automatically | Ordinary upgrade/sync retains the wrong Zoned type while remote revisions are unchanged; an actual remote revision refresh corrects it. | **Unresolved.** Requires a safe versioned metadata refresh that preserves dirty edits/outbox and resumes after interruption. The full reproduction and proposed regression are in the [floating CalDAV report](../artifacts-acceptance-2026-09-09/reports/floating-caldav-fix.md). |

## Remaining release gates

- Reconcile the first-use this-and-future capability design and its documented
  behavior. Keep any unverified provider capability pending.
- Correct cached floating CalDAV metadata during upgrade without relying on a
  remote edit or losing queued local changes; verify unchanged-etag, offline
  and interrupted-refresh cases.
- Complete Google external-account consent/refresh/restart and applicable
  primary/secondary/guest/RSVP/deletion tests; live Fastmail authorization and
  its matrix are unavailable without controlled accounts. Existing Google
  verification status is not a substitute for these client checks.
- Finish the untested real Nextcloud scheduling/RSVP, permission, conflict and
  future-scope cases; it advertises scheduling, so those are pending, not N/A.
- Complete a clean Omarchy desktop install/upgrade/removal, actual host socket
  activation, physical Wayland/mixed-monitor and suspend/resume checks, fuller
  keyboard/focus/screen-reader coverage, dense gestures and remaining
  view-specific actions.
- Qualify the actual Flatpak desktop's browser callback/keyring/portals,
  lifecycle and update/remove behavior. Native namespace tests do not prove
  those sandbox interactions.
- Qualify the separate widget candidate in its actual Omarchy shell. Its
  acceptance and publication remain independent of the app.
- Review and commit the local fixes, build a new immutable candidate, repeat
  exact downloaded-package qualification and the affected acceptance checks,
  then satisfy [PLAN.md](PLAN.md) and the owner's final publication decision.

The successful isolated work substantially reduces the testing left for the
owner. It does not support publishing the failed RC3 binaries or declaring
every feature and provider accepted.
