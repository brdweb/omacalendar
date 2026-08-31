# OmaCalendar

OmaCalendar is a local-first, keyboard-oriented calendar for Omarchy Linux. Its
aim point is the speed and interaction density of Fantastical: a complete
desktop calendar without tasks, booking services, natural-language entry, or an
OmaCalendar cloud service. An optional Quickshell companion is developed and
released independently.

> [!WARNING]
> `1.0.0-alpha` is an unsupported evaluation prerelease. Automated suites
> exercise the local database, provider mutation machinery, desktop, IPC,
> and reminders, but the full live-provider and stable owner-acceptance
> matrices are not complete. Use test or disposable calendars and never make an
> alpha build the only copy of important calendar data.

## 1.0 scope

- Writable device-only, Google Calendar, and generic CalDAV calendars.
- Read-only HTTPS/webcal subscriptions and ICS import/export.
- Agenda, day, week, month, and year views.
- Timed, all-day, multi-day, recurring, invitation, RSVP, alarm, and conflict
  workflows.
- Calendar sets, indexed search, keyboard navigation, drag/drop, resize, undo,
  and explicit provider/error states.
- Localized date/time presentation in an English interface.

The optional Omarchy Quickshell companion uses the same local daemon but has an
independent version and release path.

For a concise installation and first-run walkthrough, see
[Getting started](docs/GETTING_STARTED.md).

Tasks, Microsoft/Exchange, hosted scheduling, weather, maps, travel time,
attachments, video-provider authorization, and remote calendar creation are
not part of the 1.0 scope. See the decision-complete [1.0 roadmap](docs/PLAN.md)
for the full boundary and release gates.

## Alpha status

The app is being qualified as `1.0.0-alpha` using IPC 2 and database schema 2.
Local automated coverage currently includes the schema transition,
daemon and provider contracts, local event/calendar workflows, reminders,
recurrence, search, import/export, conflict handling, and desktop models. A
staged `/usr` install and uninstall also pass. Narrow live development checks
have passed against an isolated Radicale 3.7.8 instance and a public NASA HTTPS
ICS feed.

The current local GCC build passes all 21 tests. Earlier clean matrices passed
20 tests with GCC 16.2.1 Release and Clang 22.1.8 RelWithDebInfo under
warnings-as-errors, plus a desktop-app-enabled Clang 22.1.8 Debug build under
ASan and UBSan with no diagnostics. QML lint is clean, automated desktop smoke
tests pass at scale factors 1, 1.25, and 2. The enforced
100,000-event run measured p95 latency of 38.904 ms for agenda, 4.426 ms for
indexed search, 87.223 ms for a full widget snapshot, and 0.403 ms for an
unchanged snapshot in this development workspace.

These results do not qualify stable 1.0. Live Google writes, the full Radicale
matrix, Nextcloud, Fastmail, authenticated ICS, a clean current-Omarchy VM,
complete desktop workflow testing, clean-checkout CI/AUR builds, and the final
owner acceptance pass remain open.
The detailed evidence and unchecked gates are maintained in [the implementation
plan](docs/PLAN.md).

## Architecture

`omacalendard` is the only process allowed to write the SQLite database, access
credentials, or communicate with providers. The Qt Quick application and
`omacalendarctl` are clients of its versioned user-local IPC API. The
optional Quickshell plugin lives in the separate
`omacalendar-widget` repository and has no database, credential, or provider
access. Its releases are independent and compatibility is negotiated through
the IPC protocol and advertised capabilities.

```text
Local / Google / CalDAV / ICS
              |
        omacalendard ---- Secret Service
              |
          SQLite + local IPC
              |
       +------+------+
       |             |
    desktop app   Quickshell widget
```

Calendar views read cached data and do not wait for a provider. Credentials
belong in Secret Service and are never stored in SQLite. More detail is in
[the architecture guide](docs/ARCHITECTURE.md) and [IPC documentation](docs/IPC.md).

Installed builds enable `omacalendard.socket`. The desktop UI does not need to
be open: the first widget or CLI connection starts the daemon on demand, and
the widget's initial snapshot comes from the local cache rather than waiting
for a provider sync.

## Build from source

OmaCalendar uses C++20, CMake 3.28+, Ninja, Qt 6.8+, libical 4.0+, and Secret Service.
On Omarchy/Arch Linux:

```bash
omarchy pkg add cmake ninja gcc qt6-base qt6-declarative \
  qt6-networkauth libical libsecret pkgconf
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
ctest --test-dir build --output-on-failure
packaging/release/check-qmllint.sh build
```

Run the development binaries without installing:

```bash
./build/omacalendard &
./build/src/app/omacalendar
./build/omacalendarctl system.info
```

Exact binary locations can vary with the CMake generator. For a staged
development package installation configured for `/usr`:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DOMACALENDAR_VERSION_SUFFIX=-alpha
cmake --build build-release --parallel
DESTDIR="$PWD/stage" cmake --install build-release
packaging/release/verify-install.sh "$PWD/stage"
```

The systemd user unit is generated for the CMake configure-time prefix. Do not
configure for `/usr/local` and later override the install prefix to `/usr`.
After installing, activate the on-demand backend with:

```bash
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
```

## Testing providers

- [Google Calendar credential checkpoint](docs/GOOGLE_TESTING.md)
- [CalDAV provider matrix](docs/CALDAV_TESTING.md)

Never place credentials in the repository, fixtures, logs, issue reports, or
shell history. Live-provider suites must use dedicated test accounts.

## Project policies

- [Contributing](CONTRIBUTING.md)
- [Security](SECURITY.md)
- [Support and current limitations](SUPPORT.md)
- [Compatibility matrix](docs/COMPATIBILITY.md)
- [Backup and recovery](docs/BACKUP_AND_RECOVERY.md)
- [Privacy](docs/PRIVACY.md)
- [Release procedure](docs/RELEASE.md)
- [Uninstall and data removal](docs/UNINSTALL.md)
- [Changelog](CHANGELOG.md)

The release workflow produces checksummed archives, an SPDX SBOM, and GitHub
artifact attestations. `1.0.0-alpha` uses its own non-production gate and is
always marked as a GitHub prerelease; stable `1.0.0` remains blocked until every
stable release gate and the owner acceptance pass are complete.

## Project website

- [OmaCalendar project site](https://brdweb.github.io/omacalendar/)
- [Privacy policy](https://brdweb.github.io/omacalendar/privacy.html)
- [Terms of use](https://brdweb.github.io/omacalendar/terms.html)

OmaCalendar is released under the [MIT License](LICENSE).
