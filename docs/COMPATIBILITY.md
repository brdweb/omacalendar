# Compatibility

| App version | IPC | Schema | Distribution |
|---|---:|---:|---|
| `2.0.0` | 2.2 | 3 | Current x86-64 Arch/Omarchy |
| `1.1.1` | 2.1 | 2 | Previous x86-64 Arch/Omarchy release |
| `1.1.0` | 2.1 | 2 | Earlier x86-64 Arch/Omarchy release |
| `1.0.0` | 2.0 | 2 | Earlier x86-64 Arch/Omarchy release |

2.0.0 migrates the calendar database from schema 2 to 3 the first time its
daemon opens it. The migration is one-way: an earlier daemon refuses a schema-3
database, so back up before upgrading if you may need to go back (see
[backup and recovery](BACKUP_AND_RECOVERY.md)). Connected Google accounts are
asked to sign in again once, because 2.0.0 also requests the
`calendar.freebusy` and `tasks` permissions.

The companion widget 0.1.0 uses IPC 2.0 and remains compatible with the
additive IPC 2.1 and 2.2 revisions. It requires Omarchy 4.0.0 or newer and Quickshell
0.3.1 or newer. Both connect to the native socket-activated service. Previous
releases retain their compatibility information in their signed source tags.

Google Calendar uses the desktop browser for OAuth. Connected accounts need
a functioning Secret Service keyring. CalDAV recurrence and invitation
capabilities depend on the server; this-and-future RSVP is disabled.
ICS subscriptions are read only. Personal is a built-in local writable calendar.
