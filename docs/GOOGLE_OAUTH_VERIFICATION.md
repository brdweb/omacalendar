# Google OAuth verification package

This document is the maintainer checklist and copy source for the production
Google Auth Platform review. It intentionally contains no OAuth client ID,
secret, token, private calendar data, or test-account address.

## Public application identity

Use these values consistently in Google Cloud and the shipped application:

- Application name: **OmaCalendar**
- Homepage: <https://brdweb.github.io/omacalendar/>
- Privacy policy: <https://brdweb.github.io/omacalendar/privacy.html>
- Terms of use: <https://brdweb.github.io/omacalendar/terms.html>
- Support and source: <https://github.com/brdweb/omacalendar>
- Authorized domain: `brdweb.github.io`

The consent screen must use a project-owned support address and developer
contact. Upload the square OmaCalendar application icon without including user
data or third-party branding. Verify domain ownership with the same Google
account that owns the Cloud project before requesting branding review.

## Requested scopes and justifications

OmaCalendar requests these three scopes and no broader Calendar scope:

### `https://www.googleapis.com/auth/calendar.events`

OmaCalendar is a calendar client. It uses this scope to display, create, edit,
move, RSVP to, and delete events in calendars the user selects. Event changes
made in OmaCalendar are synchronized to Google Calendar; remote changes are
cached locally for responsive and offline calendar views. The application does
not use Calendar data for advertising, analytics, or an OmaCalendar cloud
service.

### `https://www.googleapis.com/auth/calendar.calendars`

The calendar settings UI lets a user permanently delete an owned, non-primary
secondary Google calendar after explicit confirmation. This scope is used only
for that user-initiated operation. OmaCalendar does not use it to create or
rename Google calendars, delete a primary calendar, or delete a calendar the
user does not own.

### `https://www.googleapis.com/auth/calendar.calendarlist.readonly`

OmaCalendar lists the calendars available to the signed-in user so they can
choose visibility, identify writable/read-only calendars, select a default
calendar, and preserve provider names and colors. Read-only access is
sufficient because list membership is not modified by the application.

## Privacy and storage statements

- Provider traffic goes directly between the local daemon and Google APIs.
- OmaCalendar has no hosted service and does not sell or share Calendar data.
- Calendar data is cached in a user-local SQLite database for offline use.
- OAuth refresh tokens are stored in the desktop Secret Service, not SQLite.
- The desktop authorization flow uses a loopback redirect, per-request state,
  and PKCE-S256. A distributed desktop build must not rely on a confidential
  client secret.
- Disconnecting an account removes its stored refresh token. The removal dialog
  lets the user either retain downloaded calendar data for offline reference
  (the default) or remove the account's local cached data at the same time.

## Demonstration video checklist

Record one unedited, privacy-safe walkthrough using a dedicated test account and
synthetic calendar/event names. Keep the browser address bar and application
window visible enough for a reviewer to understand the flow, but do not expose
tokens, callback query parameters, client credentials, personal account data,
or unrelated browser tabs.

Show, in order:

1. the OmaCalendar homepage, privacy policy, and application identity;
2. **Accounts & settings** and **Continue with Google in browser**;
3. Google's consent screen showing the exact requested permissions;
4. return to OmaCalendar with the account marked connected;
5. calendar discovery and the writable/read-only distinction;
6. creation, edit, move, and deletion of a synthetic timed event;
7. explicit-confirmation deletion of a pre-created synthetic, owned secondary
   calendar, including the guard against primary or non-owned calendars;
8. app and daemon restart with the account still connected and cached events
   visible; and
9. account disconnect, the retain/remove-cache choice, and removal of local
   cached data when that option is explicitly selected.

Upload the recording to a reviewer-accessible URL permitted by Google and use
that URL only in the verification form. Do not commit the consent recording to
the repository if it contains account-identifying browser UI.

## Submission and post-approval checks

Before selecting **Submit for verification**:

- reconcile every warning about enabled OAuth client secrets;
- confirm Branding, Audience, and Data Access show the production values above;
- confirm the app is External and In production;
- confirm only the three documented scopes are declared;
- review the final scope copy and video from the reviewer-accessible URL; and
- save a redacted screenshot of the verification summary for the release
  acceptance record.

After Google approves the request, authorize a new external test user who is not
on a test-user allowlist. Acceptance requires no unverified-app bypass, a
completed code exchange, a refresh token persisted in Secret Service, calendar
discovery, one event write round trip, and reconnection after app/daemon restart.

Google's approval is an external release gate. Repository and package
publication cannot make an unverified OAuth consent screen suitable for a
public beta.
