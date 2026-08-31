# Getting started

OmaCalendar `1.0.0-alpha` is an evaluation release for Omarchy Linux. Back up
important calendar data before testing it, and do not make the alpha your only
copy of an important device-only calendar.

## Install from source

Install the build dependencies and compile the application:

```bash
omarchy pkg add cmake ninja gcc qt6-base qt6-declarative \
  qt6-networkauth libical libsecret pkgconf
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
```

Start OmaCalendar from the Omarchy application launcher or run:

```bash
omacalendar
```

## Connect a calendar account

Open **Accounts & settings**, then choose a provider:

- **Google Calendar:** enter the Desktop OAuth client ID for the configured
  Google Cloud project, select **Continue with Google in browser**, and finish
  consent in the browser. Existing accounts may require one reauthorization
  after the requested scopes change.
- **CalDAV:** enter the server URL and credentials or app password supplied by
  the provider.
- **Local calendar:** create events without connecting a remote provider.

The first provider synchronization runs in the background. Cached calendars
remain usable while the desktop window is closed because the user-local daemon
is started on demand by its systemd socket.

## Everyday use

- Switch among Agenda, Day, Week, Month, and Year from the view selector.
- Use **New event** to create an event in the configured default calendar.
- Manage calendar visibility, ordering, colors, defaults, and deletions under
  **Accounts & settings**.
- Search from the toolbar. Invitation actions and provider synchronization are
  processed in the background.

The optional Omarchy widget is installed separately from the
[`omacalendar-widget`](https://github.com/brdweb/omacalendar-widget) repository.
It uses the same daemon and cached data but follows its own release versions.

## Data and troubleshooting

OmaCalendar stores its database under
`$XDG_DATA_HOME/omacalendar` (normally `~/.local/share/omacalendar`) and stores
provider credentials in Secret Service rather than SQLite. See:

- [Google test setup](GOOGLE_TESTING.md)
- [CalDAV test setup](CALDAV_TESTING.md)
- [Backup and recovery](BACKUP_AND_RECOVERY.md)
- [Compatibility and known limits](COMPATIBILITY.md)
- [Uninstall and data removal](UNINSTALL.md)

For a basic daemon check, run:

```bash
omacalendarctl system.info '{}'
```
