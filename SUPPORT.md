# Support and current limitations

`1.0.0-beta.1` is the last public prerelease. The qualified `1.0.0-rc.5` draft
is prepared for owner testing, with no production support commitment. Future
releases produce and support only a native x86-64 Arch package for current
Arch/Omarchy; instructions are in [Install](docs/INSTALL.md).
Calendar reading has been owner-tested. An isolated Radicale 3.7.8 development
slice has also exercised writes, offline recovery, remote pull/delete,
keep-remote conflict resolution, recurrence instances, and multiple alarms; a
public NASA HTTPS ICS feed passed sync/restart/manual refresh. These are partial
checks only: the complete provider, invitation, import/export, and desktop
acceptance matrices remain open. The optional Quickshell companion is supported
and released separately by its own project.

Use beta builds only with test or disposable calendars. Do not use one as the
only copy of important data. Keep provider
backups where available and make a local backup before changing builds or
schemas. See [backup and recovery](docs/BACKUP_AND_RECOVERY.md).

## Getting help

Use GitHub Issues for reproducible bugs and focused feature discussions. Include:

- the commit or version from `omacalendarctl system.info`;
- Omarchy, Qt, and provider/server versions;
- the affected workflow and whether it reproduces with a local test calendar;
- sanitized daemon/application logs; and
- exact steps with synthetic event data.

Do not include credentials, private endpoints, calendar/event text, attendee
addresses, or raw personal ICS files. Report security concerns privately as
described in [SECURITY.md](SECURITY.md).

## Target support for 1.0

- Current stable Omarchy on x86-64.
- The Qt, libical, libsecret, and systemd versions shipped by current Arch.
- Google Calendar, Radicale, Nextcloud, and Fastmail CalDAV after their complete
  live acceptance matrices pass.
- Device-only calendars and standards-compliant HTTPS/webcal subscriptions.

Microsoft/Exchange, mobile platforms, macOS, Windows, remote calendar creation,
tasks, and hosted scheduling are outside the 1.0 support boundary. Exact tested
versions and IPC compatibility are maintained in
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).
