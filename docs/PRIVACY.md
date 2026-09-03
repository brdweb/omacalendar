# Privacy and local data handling

The canonical public policy is the
[OmaCalendar privacy policy](https://omacalendar.brdweb.com/privacy.html).
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

## Sensitive-data protections

- Google authorization uses a loopback redirect, unique per-request state, and
  PKCE-S256. Google Calendar API and token traffic goes directly to Google's
  HTTPS endpoints and is protected in transit with TLS.
- Refresh tokens and provider passwords are stored through the Linux desktop
  Secret Service, which restricts access to the signed-in user and applies the
  protection provided by that user's keyring. Access tokens are held in process
  memory rather than persisted in the calendar database.
- Application data, cache, configuration, and runtime directories are created
  with owner-only permissions. The SQLite database and its sidecar files are
  also restricted to the operating-system user.
- Credentials are omitted from SQLite, logs, persisted IPC state, IPC
  presentation data, and diagnostic exports. A CalDAV or subscription password
  entered during account setup travels once over the owner-only local IPC
  socket to the daemon for storage in Secret Service; it is not retained in an
  interprocess message.

The SQLite calendar cache is not separately encrypted by OmaCalendar. Its
protection also depends on the operating-system account, keyring, filesystem,
and device. Device encryption and a strong login password are recommended,
especially on shared or portable computers.

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
