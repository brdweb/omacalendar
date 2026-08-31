# Changelog

All notable changes to OmaCalendar are recorded here. The project follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and will use Semantic
Versioning once public releases begin.

## [Unreleased]

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

[Unreleased]: https://github.com/brdweb/omacalendar/compare/v1.0.0-alpha...HEAD
[1.0.0-alpha]: https://github.com/brdweb/omacalendar/releases/tag/v1.0.0-alpha
