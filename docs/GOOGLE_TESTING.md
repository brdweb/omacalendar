# Google Calendar credential test guide

This guide is for the pre-release owner test. Source builds do not contain an
OAuth credential. Paste the client ID from a Google Desktop OAuth client in a
test project, or provide `OMACALENDAR_GOOGLE_CLIENT_ID` and the optional
`OMACALENDAR_GOOGLE_CLIENT_SECRET` in the app environment. Installed desktop
applications are public OAuth clients, but keeping deployment configuration
out of source prevents accidental reuse and makes secret scanning meaningful.
PKCE protects each authorization flow. The resulting refresh token is stored
by the desktop Secret Service (`secret-tool`) and is never written to the
calendar database.

## 1. Project configuration

In a disposable Google Cloud project, enable the Google Calendar API and
create a **Desktop app** OAuth client. Configure the consent screen for the
accounts in the acceptance test. OmaCalendar requests only these scopes:

- `https://www.googleapis.com/auth/calendar.events`
- `https://www.googleapis.com/auth/calendar.calendars`
- `https://www.googleapis.com/auth/calendar.calendarlist.readonly`

The current app uses `calendar.calendars` only to delete an owned, non-primary
secondary calendar after explicit confirmation. It does not create or rename
Google calendars. After this scope is added to an existing installation, each
previously connected Google account must complete **Reauthorize** once.

Google's current desktop-app setup is also described in its
[Calendar quickstart](https://developers.google.com/workspace/calendar/api/quickstart/go).
Google may limit refresh-token lifetime while an external consent screen is in
Testing status, so account for that during a long-running acceptance pass.
Add every acceptance-test Google account under **Google Auth Platform →
Audience → Test users**. The unverified-app warning is expected while the app
is in Testing; it does not indicate that the loopback callback failed.

## 2. Build and launch OmaCalendar

On Omarchy/Arch, the intended development dependencies are:

```bash
omarchy pkg add cmake ninja gcc qt6-base qt6-declarative qt6-networkauth libical libsecret pkgconf
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/src/app/omacalendar
```

The app starts the sibling development daemon automatically. If an installed
systemd user service is already running, the app connects to that service
instead.

## 3. Connect the test account

1. Open **Accounts & settings**.
2. Under **Google Calendar**, optionally enter an account label.
3. Paste the Desktop OAuth **client ID** shown in Google Cloud, then select
   **Continue with Google in browser**. If the app was launched with deployment
   credentials in its environment, the client-ID field is already configured.
4. Complete consent in the system browser. The callback uses a random
   `127.0.0.1` loopback port, PKCE-S256, and a per-request state value.
5. Return to OmaCalendar. The account should change from `authorizing` to
   `connected`; its selected calendars and events should then appear.

If an existing account says **Reauthorization required**, expand that account
and use **Reauthorize**, then complete the browser flow again. A successful
browser callback is only the first half of authorization: OmaCalendar must also
exchange the authorization code and save the refresh token before the account
changes to `connected`.

The implementation follows Google's recommended
[OAuth flow for installed desktop apps](https://developers.google.com/identity/protocols/oauth2/native-app).
Do not paste the browser callback URL anywhere; the app handles it locally.

## 4. Remove the unverified-app warning for a public release

The repository provides the public pages needed for Google's OAuth review:

- application home: `https://brdweb.github.io/omacalendar/`
- privacy policy: `https://brdweb.github.io/omacalendar/privacy.html`
- terms of use: `https://brdweb.github.io/omacalendar/terms.html`

Before submitting, publish those pages, verify ownership of the authorized
domain in Google Search Console, and make the product name, support email,
homepage, privacy-policy URL, and requested scopes match the application.
Add all three scopes above under **Google Auth Platform → Data Access** before
testing the updated consent flow.
Because the Calendar scopes are sensitive, prepare a short screen-recording
that shows the complete browser consent flow and how each requested permission
is used. In Google Auth Platform, change the External app from Testing to In
production, open **Verification Center**, select **Prepare for verification**,
provide the scope justifications and video, and submit it for Google review.

The project owner must perform the Cloud Console submission from the account
that owns the OAuth project. Google, not the application build, removes the
warning after approval. Keep the app in Testing with explicitly listed test
users until the public-facing pages and consent-screen details are live.

## 5. Test matrix

Use a disposable calendar if possible. For each check, allow a sync cycle or
press **Sync now**.

| Check | Action | Expected result |
|---|---|---|
| Discovery | Connect an account with at least two selected calendars | Both appear with their Google names and colors |
| Remote pull | Create a timed event in Google Calendar | Event appears in the matching local day |
| Local create | Create a timed event in OmaCalendar | Event appears remotely with the same calendar, time, title, location, and notes |
| All-day | Create an all-day event in each direction | Date is unchanged and no midnight/time-zone shift occurs |
| Update | Change title, time, location, and notes locally | Remote event updates once, without a duplicate |
| Delete | Delete a locally created event | Remote event is deleted and stays absent after another sync |
| Secondary calendar deletion | Delete a pre-created, owned, non-primary test calendar from settings and confirm the warning | The Google calendar and its events are removed; primary and non-owned calendars are not offered as deletable |
| Recurrence | Create a recurring event remotely | Instances appear in the requested local date range |
| Exception | Move or cancel one remote occurrence | Only that occurrence changes or disappears locally |
| Offline outbox | Stop networking, make an edit, restore networking, press Sync | Local edit remains visible and later drains to Google |
| Restart | Close the app and daemon, then relaunch | No consent prompt; refresh token is loaded from the keyring |
| Disconnect | Remove the account from settings, first retaining downloaded data and then repeating with cache removal selected | The refresh token is removed in both cases; cached data follows the explicit retain/remove choice |

## 6. Safe diagnostics

The generic CLI can inspect status without displaying credentials:

```bash
./build/omacalendarctl system.info
./build/omacalendarctl accounts.list
./build/omacalendarctl sync.status
```

Before sharing output, still review it: account labels, principals, calendar
names, event titles, and local identifiers may be personal even when tokens are
absent. Never share keyring contents, a browser callback URL, the SQLite
database, or raw provider payloads.

Local state lives at:

```text
~/.local/share/omacalendar/calendar.sqlite3
$XDG_RUNTIME_DIR/omacalendar/daemon.sock
```

Keyring items are labeled `OmaCalendar client-secret` and
`OmaCalendar refresh-token`.

## 7. What to report

Report the failed matrix row, local/remote calendar type, approximate event
shape (timed, all-day, recurring, or exception), and the redacted status/error
message. Replace personal titles, email addresses, IDs, URLs, and ETags with
placeholders. Do not send credentials or tokens.
