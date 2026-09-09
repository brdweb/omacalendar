# OmaCalendar

A native, local-first calendar for **Omarchy Linux**. Bring Google Calendar,
CalDAV, device calendars and ICS subscriptions together in a fast Qt desktop
app, with an optional Quickshell bar widget.

[Download 1.0.0](https://github.com/brdweb/omacalendar/releases/tag/v1.0.0) ·
[Website](https://omacalendar.brdweb.com/) · [Installation](docs/INSTALL.md) ·
[Support](SUPPORT.md)

![OmaCalendar month view](docs/screenshots/desktop-month.png)

## Features

- Agenda, day, week, month and year views.
- Event editing, recurring events, reminders and invitations.
- Calendar sets, visibility controls and calendar colors.
- Google Calendar, CalDAV, local calendars and read-only ICS subscriptions.
- Local cached data for responsive offline use; queued changes sync on reconnect.
- Optional [Omarchy widget](https://github.com/brdweb/omacalendar-widget) with
  an agenda, upcoming events, compact editing and read-only event details.

## Install

The **native x86-64 Arch package** is the supported binary distribution.
Download and verify it using the [installation guide](docs/INSTALL.md).
The package includes the app, background service, CLI and widget setup helper.
The widget requires this native service; it has its own release and install steps.

## Getting started

Open **Accounts & settings** to connect a Google or CalDAV account, subscribe
to an ICS feed, or create a local calendar. **Personal** is the built-in writable
calendar stored on this device. It can be hidden but cannot be deleted.

Use calendar sets to switch between groups of calendars. Turning off **Visible**
in settings hides a calendar and its events. Existing invitations are imported
silently; later arriving invitations can notify you.

See [getting started](docs/GETTING_STARTED.md), [backup and recovery](docs/BACKUP_AND_RECOVERY.md),
[compatibility](docs/COMPATIBILITY.md), [privacy](docs/PRIVACY.md), and
[release notes](CHANGELOG.md).

## Development

Build dependencies on Arch: CMake, Ninja, a C++ compiler, Qt 6 Base,
Declarative and NetworkAuth, libical, libsecret, pkgconf and systemd.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
packaging/release/check-qmllint.sh build
```

See [architecture](docs/ARCHITECTURE.md), [IPC](docs/IPC.md),
[release procedure](docs/RELEASE.md), and [security reporting](SECURITY.md).

## License

[MIT](LICENSE).
