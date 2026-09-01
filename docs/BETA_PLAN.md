# OmaCalendar beta release plan

The published alpha releases are the feature baseline:

- OmaCalendar app: `v1.0.0-alpha`
- OmaCalendar widget: `v0.1.0-alpha`, on an independent release path

The first app beta will be `v1.0.0-beta.1`. The first widget beta may be
`v0.1.0-beta.1`; the two versions are deliberately not coupled. Beta is a
reliability, packaging, and public-testing milestone rather than a new feature
cycle. A release-blocking defect may change code, but new views, providers, and
broad interaction changes are deferred.

The app's release-time evidence belongs in
[`releases/1.0.0-beta.1.md`](releases/1.0.0-beta.1.md). This document defines
the work; the acceptance record decides whether the exact commit may be tagged.

## 1. Public identity and Google OAuth

- [x] Publish the project homepage, privacy policy, terms, support channel, and
  source repository under one consistent OmaCalendar identity.
- [x] Request only the three Calendar scopes used by the application and
  document why each is necessary.
- [ ] Verify the Google Auth Platform branding and authorized domain.
- [x] Reconcile the Cloud Console's OAuth-client secret warning. Google's
  Desktop client currently requires its generated shared value during token
  exchange. Treat the client ID and that value as extractable public
  application configuration, while PKCE protects each authorization code.
- [ ] Record a privacy-safe demonstration of browser consent, account
  connection, calendar discovery, event CRUD, and secondary-calendar
  management.
- [ ] Submit branding and data-access verification, then record Google's
  approval and confirm that a new external user sees no unverified-app bypass.

The scope descriptions and recording checklist are in
[`GOOGLE_OAUTH_VERIFICATION.md`](GOOGLE_OAUTH_VERIFICATION.md). Browser success
alone is not acceptance: the app must exchange the code, persist the refresh
token in Secret Service, discover calendars, sync, and reconnect after restart.

## 2. Current-Omarchy owner acceptance

The maintainer's ongoing use of the alpha runtime counts as the real-world soak;
there is no arbitrary new calendar-day delay and no telemetry requirement. The
runtime tree on `main` was unchanged after the alpha tag as of 2026-08-31. The
acceptance record should identify the version/date range and summarize the
workflows actually exercised.

Before the beta tag, repeat a focused pass on the exact candidate covering:

- timed, all-day, multi-day, and recurring create/edit/move/delete workflows;
- a primary and at least one secondary Google calendar;
- invitation action and disappearance from the pending-invites view;
- offline/reconnect, daemon restart, desktop restart, and token persistence;
- background initial sync and invitation actions without foreground UI stalls;
- Agenda, Day, Week, Month, and Year navigation, overlapping events, search,
  default-calendar selection, calendar reordering, and permitted deletions;
- widget calendar filtering, agenda scrolling from today, view switching,
  event editing, app focus, and daemon-on-demand behavior; and
- repeated app/widget shutdown with no crash, data corruption, duplicate
  window, or duplicate remote mutation.

Any reproducible data-loss, credential-exposure, critical, or high-severity
defect blocks beta until it is fixed and the affected workflow passes again.
Preserve a matching coredump and build identity for every crash.

## 3. Upgrade, recovery, and removal

- [ ] Install the immutable `v1.0.0-alpha` release artifact on an isolated
  current-Omarchy test profile and create representative synthetic state.
- [ ] Back up the database and settings with the documented procedure.
- [ ] Upgrade in place to the exact beta candidate and confirm events,
  accounts, token reuse, calendar visibility/order/colors/default, reminders,
  and widget behavior survive.
- [ ] Restore the backup in the documented supported direction. Do not claim
  in-place database downgrades are supported.
- [ ] Uninstall both source and binary packages and verify every package-owned
  path is removed while user data remains unless explicitly requested.

## 4. Automated release qualification

The exact committed candidate must pass:

- current Arch GCC and Clang warning-as-error builds;
- ASan/UBSan, IPC framing/reconnect, provider, recurrence, reminder, and QML
  desktop suites;
- the enforced 100,000-event performance harness;
- QML lint, formatting, desktop/AppStream validation, dependency review, and
  complete-history secret scanning;
- staged `/usr` install, socket activation, app/daemon smoke tests, and
  manifest-driven uninstall; and
- a focused security regression review covering the previously remediated IPC,
  ICS, URI dispatch, untrusted text, recurrence, credential, and widget
  activation boundaries.

The manually dispatched release rehearsal must first build the complete
candidate with the protected Google Desktop client configuration without
creating a tag or release. The signed tag workflow must then create immutable
source and binary archives, `SHA256SUMS`, an SPDX SBOM, and GitHub
provenance/SBOM attestations. Download and independently verify the draft
assets before publication.

## 5. Documentation, screenshots, and public feedback

- [x] Add real, privacy-safe screenshots generated from an isolated profile
  containing only synthetic events. Include the desktop month, week, agenda,
  event editor/settings, and widget views where each adds useful information.
- [x] Keep build, first-run, backup/recovery, uninstall, provider-test,
  compatibility, support, security, privacy, and release documentation in the
  repository.
- [x] Add structured bug and provider-test issue forms that explicitly forbid
  credentials and private calendar data.
- [ ] Review README, website, AppStream metadata, changelog, known limitations,
  and release notes against the exact beta behavior.

## 6. Beta release and Arch package publication

1. Complete every pre-tag and external-approval row in the beta acceptance
   record.
2. Update the version suffix, dated changelog, AppStream release entry, and
   compatibility row to `1.0.0-beta.1` in the final candidate commit.
3. Create and push a signed annotated `v1.0.0-beta.1` tag. The workflow creates
   a draft GitHub prerelease; it does not publish automatically.
4. Verify downloaded checksums, provenance, SBOM attestation, exact-archive
   installation, provider write round trip, restart, and uninstall, then
   publish the draft.
5. Build a native `omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst` from the already
   validated staged `/usr` tree. Publish it with the GitHub release checksums,
   SBOM, and provenance so it never depends on a mutable branch or circular
   download from an unpublished release.
6. Install that exact asset with `yay -U` (or `pacman -U`) on a clean
   current-Omarchy profile
   and repeat the version, launch, socket, provider-reconnect, and uninstall
   smoke pass.
7. Keep `omacalendar` and `omacalendar-bin` AUR recipes rendered and clean-build
   ready. Arch `pkgver` is `1.0.0beta1` so it sorts below `1.0.0`, while upstream
   tags and filenames retain `1.0.0-beta.1`.

New AUR account registration was suspended when beta packaging was prepared, so
the public beta does not depend on an AUR submission that the maintainer cannot
create. The GitHub release package is the initial Omarchy installation path.
When AUR registration reopens or an existing trusted maintainer adopts the
recipes, regenerate `.SRCINFO`, re-run clean chroot and `namcap` checks against
the immutable release, then publish the two AUR package bases. A custom pacman
repository is not required for the first beta.

## 7. Independent widget beta and marketplace submission

The widget may advance independently after its repository passes the current
Omarchy integration suite and real-daemon acceptance:

- validate the repository root with the current `omarchy plugin validate`;
- confirm clean install, update, removal, shell reload, daemon-not-running
  startup, app focus, and widget-only event editing;
- publish a root `preview.png` captured from synthetic data;
- tag and publish `v0.1.0-beta.1` only when its own acceptance record passes;
  and
- submit the public repository to the Omarchy Plugins marketplace under the
  controlled category/tags and monitor its compatibility and automated
  security-baseline results.

Marketplace validation is not a security audit. The plugin executes
unsandboxed with the user's permissions, and its dependency and removal
documentation must say so plainly.

## Beta exit decision

Publish the app beta only when every required acceptance row contains concrete
evidence and there is no unresolved critical/high issue, known corruption,
duplicate-window, shutdown-crash, foreground-sync, or OAuth-approval regression.
The owner performs a final acceptance pass against the immutable draft before
publication. Anything still pending remains a blocker rather than being
silently relabeled as a beta limitation.

## Current references

- [Google OAuth verification requirements](https://support.google.com/cloud/answer/13464321)
- [Google verification submission process](https://support.google.com/cloud/answer/13461325)
- [Arch AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines)
- [Omarchy plugin publication guide](https://plugins.omarchy.org/publish.html)
