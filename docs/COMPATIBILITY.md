# Compatibility

| App version | IPC | Schema | Distribution |
|---|---:|---:|---|
| `1.1.1` | 2.1 | 2 | Current Omapak Flatpak and x86-64 Arch/Omarchy native release |
| `1.1.0` | 2.1 | 2 | Previous x86-64 Arch/Omarchy release |
| `1.0.0` | 2.0 | 2 | Earlier x86-64 Arch/Omarchy release |

Omapak Flatpak is the recommended official route on systems with Flatpak. It
has an isolated profile and Secret Service namespace
(`application=org.omacalendar.OmaCalendar`), so it neither reads nor migrates
the native profile or `application=omacalendar` credentials. Its sandbox daemon
is owned by a launcher invocation; synchronization and reminders run only while
that launcher remains running.

The companion widget 0.1.0 uses IPC 2.0 and remains compatible with the
additive IPC 2.1 revision. It requires Omarchy 4.0.0 or newer and Quickshell
0.3.1 or newer. Both connect to the native socket-activated service. The widget
does not integrate with the Omapak sandbox daemon. Previous releases retain
their compatibility information in their signed source tags.

Google Calendar uses the desktop browser for OAuth. Omapak intentionally omits
protected Google OAuth deployment values; Flatpak users must import their own
Google Desktop OAuth credentials JSON in **Accounts & settings** before
authorizing. Connected accounts need a functioning Secret Service keyring.
CalDAV recurrence and invitation capabilities depend on the server;
this-and-future RSVP is disabled. ICS subscriptions are read only. Personal is
a built-in local writable calendar.
