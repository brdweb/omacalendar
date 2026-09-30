# Compatibility

| App version | IPC | Schema | Distribution |
|---|---:|---:|---|
| `2.0.1` | 2.2 | 3 | Current x86-64 Arch/Omarchy |
| `2.0.0` | 2.2 | 3 | Previous x86-64 Arch/Omarchy release |
| `1.1.1` | 2.1 | 2 | Earlier x86-64 Arch/Omarchy release |
| `1.1.0` | 2.1 | 2 | Earlier x86-64 Arch/Omarchy release |
| `1.0.0` | 2.0 | 2 | Earlier x86-64 Arch/Omarchy release |

2.0.0 and later migrate the calendar database from schema 2 to 3 the first
time their daemon opens it. The migration is one-way: an earlier daemon refuses
a schema-3 database, so back up before upgrading if you may need to go back
(see [backup and recovery](BACKUP_AND_RECOVERY.md)). Connected Google accounts
are asked to sign in again once, because 2.0.0 and later also request the
`calendar.freebusy` and `tasks` permissions.

The app's widget installer installs companion widget 0.2.0, qualified with
2.0.0. It uses IPC 2.0, adds a Tasks view when the daemon offers IPC 2.2, and
remains compatible with 1.x daemons. It requires Omarchy 4.0.0 or newer and
Quickshell 0.3.1 or newer. The app and the widget both connect to the native
socket-activated service. Previous releases retain their compatibility
information in their signed source tags.

Google Calendar uses the desktop browser for OAuth. Connected accounts need
a functioning Secret Service keyring. CalDAV recurrence and invitation
capabilities depend on the server; this-and-future RSVP is disabled.
ICS subscriptions are read only. Personal is a built-in local writable calendar.
