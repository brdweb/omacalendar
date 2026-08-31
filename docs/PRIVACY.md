# Privacy and local data handling

The canonical public policy is the
[OmaCalendar privacy policy](https://brdweb.github.io/omacalendar/privacy.html).
This document explains the implementation-facing data flow.

OmaCalendar has no project-operated synchronization backend, analytics,
advertising, or telemetry. The daemon communicates directly with accounts and
subscription URLs configured by the user. It caches calendar data in a local
SQLite database so views work offline.

## Data accessed

- Calendar/account metadata and events from configured Google and CalDAV
  providers.
- ICS content from subscriptions or files selected by the user.
- Event details entered locally, including guests and reminders.
- Omarchy theme/configuration needed for appearance and optional widget
  activation.

OAuth tokens and CalDAV/subscription passwords are stored through the desktop
Secret Service. They are not stored in SQLite. Provider requests necessarily
send the fields required for the calendar operation to that provider.

## Local disclosure controls

- Diagnostics must be previewed before export and redact tokens, passwords,
  event text, attendee addresses, and private provider URLs by default.
- Logs use local object identifiers and provider status codes rather than event
  content.
- Reminder and widget privacy modes can show full details, title only, or generic
  event text.
- IPC DTOs omit provider raw payloads, credentials, ETags, and mutation bodies.

## Retention and deletion

Cached and device-only calendar data remains until the user removes it. Account
disconnect behavior must explicitly offer whether to retain or remove cached
data. Provider-side retention is governed by the provider. See
[uninstall and data removal](UNINSTALL.md) for local cleanup and credential
revocation steps.
