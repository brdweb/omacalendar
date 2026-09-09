# Getting started

Install the [verified Arch package](INSTALL.md), then launch `omacalendar`.

## Connect a calendar account

Open **Accounts & settings**, then choose a provider:

- **Google Calendar:** in a release package, select **Continue with Google in
  browser** and finish consent. Source builds must first provide their Google
  Desktop client configuration. Existing accounts may require one
  reauthorization after the requested scopes change.
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
