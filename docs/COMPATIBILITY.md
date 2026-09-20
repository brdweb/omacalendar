# Compatibility

| App version | IPC | Schema | Distribution |
|---|---:|---:|---|
| `1.1.0` | 2.1 | 2 | Current x86-64 Arch/Omarchy |
| `1.0.0` | 2.0 | 2 | Previous x86-64 Arch/Omarchy release |

The companion widget 0.1.0 uses IPC 2.0 and remains compatible with the
additive IPC 2.1 revision. It requires Omarchy 4.0.0 or newer and Quickshell
0.3.1 or newer. Both connect to the native socket-activated service. Previous
releases retain their compatibility information in their signed source tags.

Google Calendar uses the desktop browser for OAuth. Connected accounts need
a functioning Secret Service keyring. CalDAV recurrence and invitation
capabilities depend on the server; this-and-future RSVP is disabled.
ICS subscriptions are read only. Personal is a built-in local writable calendar.
