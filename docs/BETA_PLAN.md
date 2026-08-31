# Beta plan

The alpha releases are the feature baseline:

- OmaCalendar app: `v1.0.0-alpha`
- OmaCalendar widget: `v0.1.0-alpha`, on its own release path

Beta is a reliability and public-testing milestone, not another feature cycle.
New views, providers, or broad UI changes are deferred unless alpha testing
finds a release-blocking problem.

## Required before the app beta

1. **Complete Google OAuth verification.** The production OAuth project must
   have verified branding and approval for every sensitive scope the app
   actually requests. The homepage, privacy policy, terms, support contact,
   scope declarations, justifications, and demonstration video must agree with
   the shipped application. A public beta must not require users to bypass
   Google's unverified-app warning.
2. **Soak the alpha on current Omarchy.** Use real Google and local calendars
   through normal daily use for at least seven days. Cover create, edit, move,
   delete, recurrence, invitations, reminders, offline/reconnect, account
   reauthorization, daemon restart, desktop restart, and widget mutations.
   Record any crash with a core dump and resolve every reproducible data-loss,
   critical, or high-severity defect.
3. **Exercise update and recovery.** Install the published alpha artifacts in a
   clean current-Omarchy environment, preserve a backup, update to a beta
   candidate, verify schema/data/token continuity, restore the backup, and
   verify documented uninstall behavior. The app must remain responsive while
   initial and invitation-triggered provider sync runs in the background.
4. **Re-run the release gates.** The exact beta commits must pass the current
   Arch GCC, Clang, sanitizer, packaging, secret-scan, performance, IPC, and
   release-artifact workflows. Downloaded checksums, SPDX SBOMs, signed tags,
   and GitHub provenance/SBOM attestations must be independently verified
   before publication.
5. **Owner acceptance.** Repeat the desktop and widget workflows that were
   exercised during alpha development. There must be no unresolved critical or
   high issue and no known corruption, duplicate-window, shutdown-crash, or
   foreground-sync regression.

## Widget beta and marketplace

The widget may advance independently after its current-Omarchy integration
suite and a real-daemon alpha soak pass. Before marketplace submission:

- run the current `omarchy plugin validate` against the repository root;
- confirm install, upgrade, removal, bar reload, daemon-not-running startup,
  app focus, and widget-only event editing on a clean Omarchy profile;
- add a representative root preview image if desired for the marketplace card;
- review the public repository URL, category, tags, dependency disclosure, and
  submission checklist.

Marketplace listing is distribution work, not a blocker for the app beta. The
submission creates a public issue in the Omarchy marketplace repository and
therefore requires a separate explicit maintainer approval immediately before
it is sent.

## Deliberately deferred beyond beta

- Ubuntu and other distribution compatibility
- Microsoft/Exchange, mobile, macOS, and Windows support
- stable AUR publication and the complete stable-provider matrix
- broad new features that are not needed to correct alpha defects

## Beta exit decision

Cut `v1.0.0-beta` only when all five app requirements above have recorded
evidence. The widget may separately cut `v0.1.0-beta` when its own soak and
marketplace-readiness checks pass. If alpha testing produces only minor visual
or documentation issues, fix those narrowly and proceed; do not delay beta for
the stable-only items above.

## Current references

- Google OAuth verification requirements:
  <https://support.google.com/cloud/answer/13464321>
- Google verification submission process:
  <https://support.google.com/cloud/answer/13461325>
- Omarchy plugin publication guide: <https://plugins.omarchy.org/publish.html>
