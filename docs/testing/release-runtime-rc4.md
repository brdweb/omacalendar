# RC4 runtime and packaging qualification — 2026-09-09

The final RC4 implementation passed substantial real-runtime qualification.
Stable acceptance remains incomplete. Its full signed draft assembly was held
by an upstream Chrome APT repository checksum mismatch, independently reproduced
outside CI; RC5 changes that build dependency and candidate metadata only.

## Source and package identity

App signed tag `v1.0.0-rc.4` targets
`fc0975917d7bb6348151baed36ea479a49c36e66`. Local SSH and GitHub tag verification
pass. PRs #18, #19 and #20 contain the runtime fixes. Final merged CI
[34382303846](https://github.com/brdweb/omacalendar/actions/runs/34382303846)
passes compiler, sanitizer, current Arch and metadata checks. Dependency review
and secret scans passed before merge; merged source secret scanning passed.

The full same-source package rehearsal
[34382450579](https://github.com/brdweb/omacalendar/actions/runs/34382450579)
passes Arch, Ubuntu 26.04 and Flatpak build/install/runtime/removal/SPDX checks.
It is a workflow-dispatch rehearsal and did not issue provenance attestations.
The signed-tag run
[34383048566](https://github.com/brdweb/omacalendar/actions/runs/34383048566)
passes Arch and Debian builds and attestations. Its Flatpak job and retry fail
before compilation at the hosted runner's unrelated Chrome package index.
No checksum or signature check was bypassed. The original signed tag is kept.

| Final-source native executable | SHA-256 |
|---|---|
| Desktop | `15d79e6c143201d1c1d3729f13a24a3736b89f9b3f90b1e3e81afabfd86a83fb` |
| Daemon | `472faa3b89783d5f0fd62396e8743cbefa5e8ca85a50b7e35f26219fc4244f11` |
| CLI | `f8a7793be03552583aa7a5647170e6b16a3aa104cfba1ce35ec813ceed7da653` |
| Widget helper | `37858643ec9158df54752e34d6e5215baef99513e9a70a7e71595246b08eb16b` |

The earlier successful rehearsal at `fcbea97` has identical daemon, CLI and
widget-helper bytes. Its provider/service/upgrade results apply to those exact
executables. Its desktop predates the dialog palette correction and does not
substitute for the final-source desktop tests below.

## Runtime results

- Fifteen real Radicale/HTTPS ICS scenario groups pass, including qualified
  recurrence scopes, all-day/floating/zoned data, all three manual conflict
  choices, remote deletion, offline restart/replay, account lifecycle, authenticated
  HTTPS errors/cache retention and ICS import/export duplicate handling.
- Real Nextcloud 34.0.3 passes eight baseline and ten expanded groups: read-only
  permissions, first-use future-scope proof, two-user local scheduling, series
  and occurrence RSVP, safe rejection of unsupported future RSVP, ETag conflicts
  and remote deletion. Email invitations and external attendees were disabled
  in the disposable two-user server.
- Floating-cache normalization preserves offline queued edits and conflict
  snapshots through interrupted/retried upgrades, and receives real server replay
  acknowledgements. The shared-resource startup failure regression is fixed.
- Twelve private real desktop-service groups pass with actual Secret Service
  and D-Bus: namespace separation, restart credential reuse, notifications and
  actions, privacy/escaping, retry after Notify error, socket handoff and private
  permissions. These use disposable sessions and profiles.
- Beta and RC3 upgrades preserve full synthetic event DTOs, detached/cancelled
  recurrence, alarm jobs, nine settings, calendar preferences, ordered/default
  sets and local/CalDAV accounts across two restarts. Restoring the older backup
  works with the older binary and leaves backup hashes unchanged. Schema-1
  private archive/reset/reconnect and failed-initialization recovery pass.
  The comparator normalizes only the previously absent false future-RSVP
  capability; all other fields remain exact comparisons.
- The final-source desktop passes 16 isolated real-pointer/AT-SPI checks at
  125%, including all five views, one-hour drag, 30-minute resize and undo,
  accessible search, deletion, URI activation and restart preservation.

## Clean Omarchy guest

The official verified Omarchy 4.0.3 image was installed into an isolated KVM
guest with Hyprland 0.56.2, Qt 6.11.2 and an actual Wayland desktop. No host
calendar profile or personal provider credential was exposed to it.

- A published-beta package baseline upgraded to RC4 with complete seeded data
  and settings retained. The final-source native package passes integrity:
  72 owned files, zero altered files.
- Ten real Wayland interaction checks pass with the final-source packaged app:
  desktop launch, all five views, exact date/time/location creation, URI editing,
  multi-day exclusive dates, accessible search/deletion and desktop `.ics`
  opening with preview/import. The preceding palette development build also
  passes the same interactions at compositor-verified 125% and 200% scale;
  those extra scale results are development evidence, not different package
  identities. The original legacy `hyprctl keyword` attempt did not change
  scale and is excluded; `hyprctl eval` plus monitor readback established the
  actual tested modes.
- Uninstall removes every package-owned file and retains every stopped profile
  byte. Reinstall preserves data, starts the backend through its enabled user
  socket (directory 0700, socket 0600), and passes CRUD/delete/undo and integrity.
- Screenshot review found pale text on a default white import dialog. PR #20
  supplies the application palette to Qt Basic dialogs and standard buttons.
  The corrected native and Flatpak previews are visibly readable.
- Widget RC4 source uses Omarchy's supported read-only-bar setter. Actual popup
  geometry and pointer/keyboard event creation pass on all four bar edges;
  rendering remains bounded at verified 1/1.25/2 virtual scale. Every tested
  runtime file and manifest matches the independently downloaded signed archive.
  Install/restore/reinstall and unchanged-baseline byte-exact config recovery
  pass. One comparison to a pre-bar-test baseline differed only in Unicode JSON
  escaping; its values were identical, and the unchanged-baseline repeat passed.

## Actual Flatpak guest

The exact final-source rehearsal bundle was installed with KDE 6.10 runtime
commit `31742ff0f905913c512ea732960bbdeffa8306dcbebca45eeee62bfed0ca03a3`.
The final signed release still needs a separately successful build and identity
check; this rehearsal bundle is not relabeled as an attested release artifact.

- Ten graphical Wayland interactions pass, including actual keyboard text
  entry in every view, URI editing, all-day dates, search/delete and `.ics`
  preview/import through the exported desktop file-forwarding/portal path.
  Qt 6.10 exposes readable/focusable text but no AT-SPI EditableText interface
  for these fields; actual Wayland keyboard input with readback was used.
- Native and Flatpak run simultaneously in two distinct Wayland windows with
  independent event stores and socket inodes. The sandbox cannot access the
  native database or daemon socket.
- The actual guest Secret Service and real loopback Radicale 3.7.8 pass
  discovery, credential namespace checks, create/edit readback, restart login
  reuse, offline replay, disconnect/reconnect and account removal. The generated
  credential is absent from app data/logs and is removed through account cleanup.
  The restart harness signals its recorded launcher PID inside its exact
  sandbox instance; terminating the outer Flatpak process was not a valid
  application-lifecycle test.
- The real guest notification service acknowledges a reminder. Closing the last
  graphical client ends the sandbox daemon; reopening preserves the exact event
  and delivered reminder without redelivery. Actual user uninstall retains all
  stopped profile bytes and reinstall reopens the same event.
- A separate installed sandbox suite also passes CRUD/undo, persistence, second
  activation, shared daemon lifetime, rendered startup and removal. A screenshot
  attempt during guest idle lock is excluded; the fixture was authenticated and
  the coexistence check repeated successfully.

## Performance and remaining gates

The final-source native daemon passes all unchanged gates on the reference
Intel Core 5 320 host with 100,000 deterministic events, five warmups and 31
samples. Test processes were pinned to CPU 1 and the disposable VM was paused
for the measurement. Agenda p95 is 36.996 ms (200 ms limit), search 4.011 ms
(250 ms), and full widget snapshot 67.206 ms (100 ms). Range/FTS plans and
expected result counts pass. Earlier contention failures remain documented in
[the performance investigation](performance-rc4.md).

Still pending: dedicated real Google/Fastmail account workflows, physical mixed
monitors and suspend/resume, complete owner accessibility/focus and widget
interaction acceptance, interactive portal picker/browser acceptance, and final
candidate artifact qualification. Google OAuth verification and marketplace
approval remain independent; neither supplies missing runtime or owner evidence.
Raw reports, failed harness attempts, screenshots and exact artifacts are
retained locally under `artifacts-rc4-release/`.
