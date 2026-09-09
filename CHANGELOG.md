# Changelog

All notable changes to OmaCalendar are recorded here. The project follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and will use Semantic
Versioning once public releases begin.

## [Unreleased]

## [1.0.0-rc.3] - 2026-09-08

### Fixed

- Align the Flatpak SPDX inventory with the actual distributed bundle. RC2
  described 61 locale files from its build stage that separate-locale export
  omitted from the bundle; RC3 requires exact downloaded file-set/hash checks.
- Use GitHub-safe Debian asset filenames such as
  `omacalendar_1.0.0-rc.3-1_amd64.deb`, retaining internal Debian version
  `1.0.0~rc.3-1` and its correct prerelease ordering. No local rename is needed.

### Changed

- Prepare a fresh app/widget RC3 pair with new builds, documentation, checksums,
  and attestations. Preserve RC1/RC2 tags and records; no old artifact is renamed
  or promoted to RC3. Runtime logic, IPC, schema, and owner gates are unchanged.

## [1.0.0-rc.2] - 2026-09-08

### Changed

- Replace the paired RC1 preparation with app `1.0.0-rc.2` and widget
  `0.1.0-rc.2` before owner artifact delivery. GitHub could not verify the
  widget RC1 tagger email; the existing signed RC1 tags remain unchanged.
- Update candidate versions, package filenames, and documentation only;
  runtime implementation and pending stable acceptance gates are unchanged.
  RC2 artifacts must be rebuilt and independently verified under their new tags.

## [1.0.0-rc.1] - 2026-09-08

### Added

- Native Ubuntu 26.04 amd64 `.deb` and an installable Flatpak bundle alongside
  the native Arch package, with independent build and installation checks.
- A stable-release preparation plan, exact-candidate acceptance record, and
  owner test checklist; candidate drafts keep pending stable gates visible.
- Installation, update, backup, removal, and sandbox documentation for each
  package format, delivered with the candidate artifacts.

### Changed

- Flatpak uses isolated data, application-instance and daemon sockets, and
  keyring identities. The native Omarchy widget continues to use the native
  daemon; sandbox and native profiles are separate.
- Release automation assembles all package formats before making a draft
  available. Stable tagging still requires completed acceptance evidence.
- Correct the minimum Qt requirement to 6.9: the OAuth implementation already
  used APIs introduced in that version. The `.deb` targets Ubuntu 26.04;
  Debian 13 and Ubuntu 24.04 users should use Flatpak.

## [1.0.0-beta.1] - 2026-09-06

### Added

- IPC 2 stability and deprecation policy in `docs/IPC.md`, documenting
  compatibility guarantees, deprecation notice, and removal criteria for the
  cross-repository protocol.
- Translatable user-facing strings: every QML view/component and the desktop
  app's C++ status/error surface now route through `qsTr()`/`tr()`, and an
  optional `lupdate_ts` CMake target (requires `qt6-tools`) regenerates
  `src/app/translations/omacalendar_en.ts`. English-only UI, no behavior
  change; no catalogue is shipped or loaded yet.
- `docs/DESIGN_TASKS_SCHEMA.md`: a design record answering whether tasks
  (`VTODO`) fit schema 2. Recommends a sibling `tasks` table, added the same
  way existing additive schema-2 repairs are, so a first tasks release would
  not need a migration. No implementation.
- A checksummed native Arch package for current Omarchy, deterministic source
  and binary archives, an SPDX SBOM, and GitHub provenance/SBOM attestations.
- Privacy-safe release screenshots, focused beta issue forms, and a public
  Google OAuth verification package under the canonical OmaCalendar identity.

### Changed

- Release builds inject the public Google Desktop OAuth client configuration
  from protected CI inputs without committing or logging it; source builds
  remain credential-free.
- Installed documentation now preserves the repository's `docs/` layout and
  includes the beta install, Google testing, recovery, release, and uninstall
  guides referenced by the packaged README.

### Fixed

- ICS subscription refreshes and imports retain the complete recurrence set
  (RRULE, RDATE, EXDATE, and EXRULE), including folded properties and RDATE-only
  events, across export and restart without duplicating complete feed bodies.
- Dense-calendar widget snapshots first query a one-day Up Next window, with
  the same 45-day fallback for sparse calendars, avoiding unnecessary event
  hydration while preserving ongoing-event and lookahead boundary behavior.
- Google authorization now completes code exchange with PKCE-S256, persists
  refresh tokens in Secret Service, discovers calendars, and reconnects after
  daemon restart with the packaged Desktop client configuration.
- Release install/uninstall validation now covers the packaged
  `omacalendar-widgetctl` integration helper as well as the app, daemon, CLI,
  desktop metadata, and user units.

### Security

- Sensitive account, provider, and event fields are excluded from generic IPC
  errors, diagnostics, and presentation-only widget snapshots.
- Bound recurrence enumeration and index exception lookup; reject incomplete
  expansions before replacing cached occurrences.
- Store CalDAV resource payloads once per resource, and enforce cumulative
  response, request, and unique-resource limits across synchronization.
- Stream ICS responses within the size limit and reject cross-origin redirects.
- Render provider text literally, restrict external event links to HTTP(S),
  and default new notification preferences to generic text.
- Pin the app's widget installer to the reviewed beta tag and exact commit.
- Enable full ELF RELRO and make package verification instructions abort on
  checksum or provenance failure.

## [1.0.0-alpha] - 2026-08-30

### Added

- A concise installation, first-run, everyday-use, data, and troubleshooting
  guide for alpha testers.
- A systemd user socket keeps the local IPC endpoint available and starts the
  daemon on demand, allowing the independently released widget to read cached
  calendar data without the desktop application running.
- Calendar settings can permanently delete custom local calendars and owned,
  non-primary Google calendars after confirmation.
- Writable calendars can be assigned as the shared default for new events in
  both the desktop application and widget.
- Initial local daemon, SQLite cache, IPC client/server, Qt Quick application,
  CalDAV adapter, Google Calendar adapter, recurrence support, and test suite.
- Development-version suffix so unreleased builds cannot be confused with a
  published package.
- GCC/Clang, sanitizer, formatting, QML lint, dependency, and secret-scanning
  workflow definitions.
- Prefix-aware systemd unit generation, desktop/MIME/URI registration, branded
  icon, AppStream metadata, CPack configuration, and source/binary AUR templates.
- Release archive, SHA-256, SPDX SBOM, and signed-attestation scaffolding.
- Public security, support, contribution, privacy, recovery, compatibility,
  release, and uninstall documentation.
- An app-only `1.0.0-alpha` prerelease track and explicit non-production
  acceptance record; the optional widget retains an independent release path.

### Fixed

- Google OAuth now requests narrow calendar-management access, migrates existing
  accounts to a one-time reauthorization prompt, and replaces raw insufficient-
  scope deletion failures with actionable guidance.
- Calendar detail cards are compact, expose colors only from an on-demand
  palette, and use drag-and-drop ordering with a floating preview and labeled
  before/after insertion target instead of numeric position fields.
- The ambiguous "Ignore provider alerts" option is now labeled "Mute invitation
  alerts" and explains that event reminders remain enabled.
- Calendar settings now use one clearly labeled default-calendar selector and
  consistently spaced preference groups instead of per-calendar default buttons.
- Desktop launches now route into the existing OmaCalendar process, restore its
  window, and request focus instead of opening duplicate application windows.
- Invalid or deleted default-calendar preferences now resolve to an available
  writable calendar, preferring the Google primary calendar.
- Google deletion tombstones are no longer presented as blank calendars.

### Changed

- Rebaselined the app roadmap around a feature-complete 1.0 desktop application
  while moving the Quickshell companion to its own project release plan.
- IPC notifications now honor per-connection topic subscriptions and reset
  subscription state on reconnect.
- Release candidates now preserve semantic prerelease suffixes in binaries,
  archives, AppStream metadata, changelogs, and draft GitHub releases.

- Route desktop-launched local `.ics` files into the validated import-preview
  workflow instead of silently ignoring the registered MIME handler.
- Force the widget to refresh ephemeral sync/auth/offline status when the
  daemon revision is otherwise unchanged.
- Avoid retaining mutable IPC receive-buffer iterators across re-entrant request
  routing, with focused disconnect and framing regressions.
- Discard truncated client frames across reconnects and cancel obsolete retry
  timers once a replacement IPC connection succeeds.

### Security

- Release automation scans committed and candidate content for secret patterns
  and emits verifiable build provenance for tagged release candidates. The
  separately documented historical OAuth incident remains a pre-tag gate.

[Unreleased]: https://github.com/brdweb/omacalendar/compare/v1.0.0-rc.3...HEAD
[1.0.0-rc.3]: https://github.com/brdweb/omacalendar/compare/v1.0.0-rc.2...v1.0.0-rc.3
[1.0.0-rc.2]: https://github.com/brdweb/omacalendar/compare/v1.0.0-rc.1...v1.0.0-rc.2
[1.0.0-rc.1]: https://github.com/brdweb/omacalendar/compare/v1.0.0-beta.1...v1.0.0-rc.1
[1.0.0-beta.1]: https://github.com/brdweb/omacalendar/compare/v1.0.0-alpha...v1.0.0-beta.1
[1.0.0-alpha]: https://github.com/brdweb/omacalendar/releases/tag/v1.0.0-alpha
