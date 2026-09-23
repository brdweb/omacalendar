# OmaCalendar

A local-first calendar for **Linux desktops**. Bring Google Calendar, CalDAV,
device calendars and ICS subscriptions together in a fast Qt desktop app. An
optional Quickshell bar widget is available with the native Arch/Omarchy
installation.

[Native Arch release 1.1.1](https://github.com/brdweb/omacalendar/releases/tag/v1.1.1) ·
[Website](https://omacalendar.brdweb.com/) · [Installation](docs/INSTALL.md) ·
[Support](SUPPORT.md)

![OmaCalendar month view](docs/screenshots/desktop-month.png)

## Features

- Agenda, day, week, month and year views.
- Event editing, recurring events, reminders and invitations.
- Series and occurrence moves between writable calendars, with confirmation
  before cross-account or cross-provider moves.
- Calendar sets, visibility controls and calendar colors.
- Google Calendar, CalDAV, local calendars and read-only ICS subscriptions.
- Local cached data for responsive offline use; queued changes sync on reconnect.
- Optional [Omarchy widget](https://github.com/brdweb/omacalendar-widget) for
  native Arch/Omarchy installations, with an agenda, upcoming events, compact
  editing and read-only event details.

## Install

The recommended official route is the **Omapak Flatpak**:

```bash
flatpak remote-add --user --if-not-exists omapak https://repo.omapak.org/omapak.flatpakrepo
flatpak install --user omapak org.omacalendar.OmaCalendar
```

Omapak has an isolated profile, does not integrate with the native widget, and
keeps background synchronization and reminders active only while its launcher
remains running. See the [installation guide](docs/INSTALL.md) for Flatpak
run, update, removal, profile, and Google-credential details.

The verified **native x86-64 Arch/Omarchy package** remains supported. Download
and verify it using the [installation guide](docs/INSTALL.md); it includes the
native background service, CLI, and widget setup helper.

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
