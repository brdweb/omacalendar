# OmaCalendar

OmaCalendar is a local-first calendar for Omarchy Linux. It combines a native
Qt Quick desktop application with a background calendar service and, in a later
phase, a theme-aware Omarchy Quickshell widget.

The first supported providers are:

- CalDAV
- Google Calendar

Microsoft calendar services are intentionally out of scope.

## Project status

OmaCalendar is under active early development. The native app, local daemon,
SQLite/offline core, CalDAV adapter, and Google Calendar adapter have reached
the Phase 4 owner-credential checkpoint. The Quickshell widget begins after
that acceptance test. The detailed roadmap and exit criteria live in
[`docs/PLAN.md`](docs/PLAN.md).

## Design principles

- One background service owns synchronization, credentials, recurrence, local
  storage, reminders, and conflict handling.
- The desktop app and shell widget are thin clients of a versioned local API.
- Opening a calendar view never waits for the network.
- Credentials are stored with the desktop Secret Service, never in SQLite.
- Calendar data stays on the local machine except when sent to the configured
  calendar provider.
- Provider differences are represented explicitly instead of silently losing
  fields.

## Planned binaries

- `omacalendard` — background service and sync engine
- `omacalendar` — Qt Quick desktop application
- `omacalendarctl` — diagnostics and automation CLI

## Building

The development stack is C++20, Qt 6.8 or newer, SQLite, Qt Network
Authorization, libical, and Secret Service/libsecret. On Omarchy/Arch:

```bash
omarchy pkg add cmake ninja gcc qt6-base qt6-declarative qt6-networkauth libical libsecret pkgconf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/src/app/omacalendar
```

Provider test instructions:

- [Google Calendar credential checkpoint](docs/GOOGLE_TESTING.md)
- [CalDAV provider testing](docs/CALDAV_TESTING.md)

Do not place provider credentials inside the repository.

## Project website

- [OmaCalendar project site](https://brdweb.github.io/omacalendar/)
- [Privacy policy](https://brdweb.github.io/omacalendar/privacy.html)
- [Terms of use](https://brdweb.github.io/omacalendar/terms.html)

## License

OmaCalendar is released under the [MIT License](LICENSE).
