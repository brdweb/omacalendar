# Compatibility

| App version | IPC | Schema | Distribution |
|---|---:|---:|---|
| `2.0.1` | 2.2 | 3 | Current Omapak Flatpak and x86-64 Arch/Omarchy native release |
| `2.0.0` | 2.2 | 3 | Previous x86-64 Arch/Omarchy release |
| `1.1.1` | 2.1 | 2 | Earlier Omapak Flatpak and x86-64 Arch/Omarchy release |
| `1.1.0` | 2.1 | 2 | Earlier x86-64 Arch/Omarchy release |
| `1.0.0` | 2.0 | 2 | Earlier x86-64 Arch/Omarchy release |

2.0.0 and later migrate the calendar database from schema 2 to 3 the first
time their daemon opens it, including an existing Omapak profile. The migration
is one-way: an earlier daemon refuses a schema-3 database, so back up before
upgrading if you may need to go back (see
[backup and recovery](BACKUP_AND_RECOVERY.md)). Connected Google accounts are
asked to sign in again once, because 2.0.0 and later also request the
`calendar.freebusy` and `tasks` permissions.

Omapak Flatpak is the recommended official route on systems with Flatpak. It
has an isolated profile and Secret Service namespace
(`application=org.omacalendar.OmaCalendar`), so it neither reads nor migrates
the native profile or `application=omacalendar` credentials. Its sandbox daemon
is owned by a launcher invocation; synchronization and reminders run only while
that launcher remains running.

The native app's widget installer installs companion widget 0.2.0, qualified
with 2.0.0. It uses IPC 2.0, adds a Tasks view when the daemon offers IPC 2.2,
and remains compatible with 1.x daemons. It requires Omarchy 4.0.0 or newer and
Quickshell 0.3.1 or newer, and connects to the native socket-activated service.
The widget does not integrate with the Omapak sandbox daemon. Previous releases
retain their compatibility information in their signed source tags.

Google Calendar uses the desktop browser for OAuth. Omapak intentionally omits
protected Google OAuth deployment values; Flatpak users must import their own
Google Desktop OAuth credentials JSON in **Accounts & settings** before
authorizing. Connected accounts need a functioning Secret Service keyring.
CalDAV recurrence and invitation capabilities depend on the server;
this-and-future RSVP is disabled. ICS subscriptions are read only. Personal is
a built-in local writable calendar.
