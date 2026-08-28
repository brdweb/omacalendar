# Google Calendar credential test guide

This guide is for the Phase 4 owner test. OmaCalendar includes its official
Google Desktop OAuth client credentials, so ordinary users do not download or
manage a credentials file. Installed desktop applications are public OAuth
clients, so the application key cannot be confidential; PKCE protects each
authorization flow. The resulting refresh token is stored by the desktop
Secret Service (`secret-tool`) and is never written to the calendar database.

## 1. Project configuration

The project owner has configured Google Cloud project `omacalendar-506917`,
enabled the Google Calendar API, and created the **OmaCalendar Linux Desktop**
OAuth client. The app is currently **External / In production** and
requests only these scopes:

   - `https://www.googleapis.com/auth/calendar.events`
   - `https://www.googleapis.com/auth/calendar.calendarlist.readonly`

Google's current desktop-app setup is also described in its
[Calendar quickstart](https://developers.google.com/workspace/calendar/api/quickstart/go).
Because the app is in production, owner refresh tokens are not subject to the
seven-day lifetime imposed on external apps in Testing status.

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
3. Select **Connect Google**.
4. Complete consent in the system browser. The callback uses a random
   `127.0.0.1` loopback port, PKCE-S256, and a per-request state value.
5. Return to OmaCalendar. The account should change from `authorizing` to
   `connected`; its selected calendars and events should then appear.

The implementation follows Google's recommended
[OAuth flow for installed desktop apps](https://developers.google.com/identity/protocols/oauth2/native-app).
Do not paste the browser callback URL anywhere; the app handles it locally.

## 4. Test matrix

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
| Recurrence | Create a recurring event remotely | Instances appear in the requested local date range |
| Exception | Move or cancel one remote occurrence | Only that occurrence changes or disappears locally |
| Offline outbox | Stop networking, make an edit, restore networking, press Sync | Local edit remains visible and later drains to Google |
| Restart | Close the app and daemon, then relaunch | No consent prompt; refresh token is loaded from the keyring |
| Disconnect | Remove the account from settings | Refresh token and local cached account data are removed |

## 5. Safe diagnostics

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

## 6. What to report

Report the failed matrix row, local/remote calendar type, approximate event
shape (timed, all-day, recurring, or exception), and the redacted status/error
message. Replace personal titles, email addresses, IDs, URLs, and ETags with
placeholders. Do not send credentials or tokens.
