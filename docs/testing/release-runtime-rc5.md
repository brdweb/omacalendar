# RC5 downloaded-artifact qualification — 2026-09-09

**Disposition: qualified draft for owner testing; stable publication is pending.**

Install app `1.0.0-rc.5` using [INSTALL.md](../INSTALL.md), with native widget
`0.1.0-rc.4` where applicable. [OWNER_TESTING.md](../OWNER_TESTING.md) contains
the full owner checklist. The tables here distinguish performed checks from
remaining acceptance; they do not turn an unperformed owner check into a pass.

## Immutable identity

- App tag `v1.0.0-rc.5`, tag object `c29995f08ab16235be73e2657d7faa85bcd47700`.
- Source commit `85ff255e0cb9302b0efffdbd58d602b271b29134`.
- Local SSH and GitHub tag verification both pass with release-key fingerprint
  `SHA256:xRvSRXTtMvXphunsVMOjzmV9P/vzdtc3L5jL9jhvjUs`.
- [Protected source review #21](https://github.com/brdweb/omacalendar/pull/21),
  [merged-source CI](https://github.com/brdweb/omacalendar/actions/runs/34388079459),
  and [complete signed package workflow](https://github.com/brdweb/omacalendar/actions/runs/34388730830) pass.
- Original `SHA256SUMS` SHA-256:
  `0803d87c982d652d16bfa3b31c76e99176ca038b6f067f1fcf33d98d623f7eb1`.

| Download | SHA-256 |
|---|---|
| `omacalendar-1.0.0rc5-1-x86_64.pkg.tar.zst` | `d9885b3505dd2943914ecfcee91d51c5d37c9e95e5f6493af67867950ffa9d2b` |
| `omacalendar_1.0.0-rc.5-1_amd64.deb` | `fc67250fc60f502610954433a6ea8c6591222b8fd7cf6ab634493ac5a7d72a03` |
| `omacalendar-1.0.0-rc.5-linux-x86_64.flatpak` | `469aff8e640d2dc1f4477bff179302552595128a9e4933358e0b7901aeabe0fe` |

All 19 original checksum entries and the actual hosted/downloaded filenames
match. Six source-constrained provenance verifications and four SPDX
attestation verifications pass, with each verified SPDX predicate exactly
equal to its downloaded document. Source and documentation archive contents
match the signed source. The separate dependency source archives match the
build-pinned libical 4.0.5 and libsecret 0.21.7 checksums.

Independent SPDX 2.3 schema, regular-file hash/coverage, unique identifiers,
relationship endpoints and package verification codes pass: Arch 53 regular
files, Debian 67, Flatpak 132. The native archive and Arch package have identical
payload files, symlinks and modes. The Flatpak inventory was checked against
the exported OSTree bundle itself.

## Runtime evidence and limits

| Area | Performed checks | Remaining acceptance |
|---|---|---|
| Source matrix | GCC, Clang, current Arch, ASan/UBSan, QML, metadata, secret scan and dependency review pass | Future source changes require appropriate new checks |
| Native packages | Signed-workflow clean Arch and Ubuntu 26.04 installation/removal; final Arch package in real Omarchy 4.0.3 Wayland guest, 74 package entries with no alteration | Owner installation and ordinary daily use |
| Native lifecycle | Beta-seeded complete event DTOs/settings retained; exact package removal preserves every stopped profile byte; reinstall and real systemd socket activation pass with 0700 directory/0600 socket | Owner accounts and personal-environment upgrade acceptance |
| Upgrade/recovery | Beta and RC3 full DTO/settings/account/cache upgrades, two restarts, immutable backup restore; schema-1 WAL and blocked initialization recovery; floating-cache/queued-edit migration | Real hosted-provider login continuity |
| Native desktop | Signed app passes 16 isolated desktop checks including pointer drag, resize and undo at 125%; ten real Wayland checks cover all five views, timed/multi-day events, search/edit/delete, desktop URI and ICS import | Complete accessibility/navigation, recurrence/invitation UI and physical display acceptance |
| Radicale / ICS | 15 transport groups, including recurrence scopes, conflicts, offline/restart, alarms, authenticated HTTPS credential rotation, import/export duplicate policies and deletion | Hosted-service variations and owner UI matrix |
| Nextcloud 34.0.3 | Eight baseline and ten extended groups: two users, sharing permissions, first-use scope proof, local invitations/RSVP, ETag conflicts and deletion | Hosted Fastmail and Google acceptance; no external invitation mail was sent |
| Desktop services | Twelve private real-D-Bus groups exercise Secret Service, notifications/actions/privacy/retry and systemd socket handoff | Physical suspend/resume and full owner reminder workflow |
| Flatpak | Exact signed bundle passes isolated sandbox verification and real Omarchy guest keyring/CalDAV/offline restart, ten Wayland checks, notification acknowledgment, final-GUI lifetime and uninstall/reinstall retention | Google browser consent/callback, full owner desktop matrix |
| Widget | Signed widget RC4 passes RC5 pairing: all 56 source files, byte-exact install/restore, rendered popup, backend recovery and one real pointer/keyboard creation; prior four-edge/virtual-scale results remain scoped | Full owner controls/physical monitors |

The exact final native daemon (`0e0c955614ffea3abb8174e78c7e285e9c96b7d4ba8e2dffe92a533b9fb698fb`),
CLI and widgetctl are byte-identical to the RC5 runtime-qualified rehearsal.
The final graphical app has a different build identity
(`60f1b86810701c0ec2a59cc7b0eb6be14dc685a1e92d5c5e9c17210b6c0f1622`)
and received its own signed-build desktop tests. The installed signed-workflow
Arch/Flatpak payloads are byte-identical to their fresh GitHub release downloads.

The unchanged 100,000-event performance harness passes with five warmups and
31 samples, CPU affinity 1, and the test VM paused: agenda p95 37.471 ms, search
4.102 ms, widget snapshot 67.946 ms. Limits remain 200/250/100 ms. This is a
recorded controlled run; earlier unpinned contention and RC4 measurements are
retained in [the RC4 report](release-runtime-rc4.md), not hidden or relabeled.

Final signed Flatpak portal checks pass in a fresh session: GTK save picker
exports the intended event/URL, Chromium loads its public fixture link, and
the GTK open picker grants the export for re-import preview. The final import
selection used actual pointer input. Earlier driver attempts failed on focus,
text selection and GTK action naming; one post-reinstall export left a portal
temporary file. Those attempts remain recorded and are not counted as passes.
The owner should repeat the file workflow after an ordinary update/reinstall.

## AUR clean-chroot qualification

The unchanged `PKGBUILD` and `PKGBUILD-bin` downloaded with RC5 pass separate
Arch devtools 1.5.1 `makechrootpkg -c` builds. `mkarchroot` created a pristine
base-devel root from the official core/extra configuration; source and binary
builds used separate clean copies inside the disposable Omarchy VM. Exact
signed-release archives were preloaded under the recipes' source-cache names
because draft URLs require authentication. `makepkg --verifysource` passed for
both before the build. No recipe was edited and no credentials entered a chroot.

The source build passes 21/21 configured CTest cases, daemon restart smoke and
QML lint. Both resulting packages pass chroot installation, file integrity,
ordinary-user IPC/version/private-permission/restart checks and removal. The
hardware timing gate remains the separately recorded final release-daemon run.
The generated `.SRCINFO` records match the recipe versions and checksums.

The source build has Google available but unconfigured: it does not receive
CI's bundled desktop-client configuration. Source users must configure their
own Google Desktop OAuth client. The published-binary recipe retains the
configuration of its verified binary archive. These successful chroot builds
do not publish either AUR package or make draft download URLs public.

## Work still required for stable publication

1. **Controlled Google and Fastmail accounts.** Complete the applicable consent,
   calendar discovery, writes/readback, recurrence, invitations, offline/restart,
   reauthorization and disconnect matrices. Delete only a disposable owned Google
   secondary calendar. Google verification is approved, but that does not prove
   the external-account workflow on this candidate.
2. **Owner desktop/widget acceptance.** Complete the unperformed portions of
   [OWNER_TESTING.md](../OWNER_TESTING.md), especially physical mixed monitors,
   suspend/resume, keyboard/accessibility and full widget actions with concurrent
   desktop changes. Virtual screens and scripted interactions cover only their
   stated scope.
3. **Stable release build.** After all applicable `PLAN.md` gates pass, prepare
   stable metadata, create fresh signed stable tags, build and independently
   qualify those newly named artifacts, and publish. RC5 remains a draft;
   the widget's main/public release branch remains separate until its own gates pass.

CalDAV this-and-future RSVP remains deliberately unavailable where scheduling
support is not established. Nextcloud retained the ranged attendee change but
delivered only one occurrence to the organizer. A storage capability pass is
not a scheduling pass; supported series/occurrence responses remain available.

The widget marketplace submission is approved/listed for its reviewed snapshot.
The dependency-documentation update still awaits external maintainer review.
A GitHub Flatpak bundle does not imply a Flathub listing, and generated AUR
recipes do not imply an AUR publication.

## Evidence handling

Original signed tags, checksum manifests and package assets are unchanged.
The draft's signed verification receipt supplements the pre-build acceptance
snapshot; it does not replace that historical file. Candidate evidence is kept
under `artifacts-rc5-release/`, with earlier failures/rehearsals under
`artifacts-rc4-release/`. Synthetic fixture credentials and the VM's private SSH
key are excluded from shareable reports. No personal profile was used for these tests.
