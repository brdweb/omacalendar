# CalDAV setup

Open **Accounts & settings**, choose **CalDAV** and enter a server
address, user name and password. OmaCalendar discovers the calendars and task
lists behind the address. HTTPS is required (plain HTTP is accepted only for
`localhost`).

Use an **app-specific password** wherever the provider offers one. It can be
revoked without changing your account password, and providers with two-factor
sign-in require it.

| Provider | Server address | Password |
|---|---|---|
| Fastmail | `https://caldav.fastmail.com/` | An app password with CalDAV access (Settings → Privacy & Security → App passwords) |
| iCloud | `https://caldav.icloud.com/` | An app-specific password from <https://account.apple.com> (Sign-In and Security → App-Specific Passwords); user name is your Apple ID email |
| Nextcloud | `https://cloud.example.com/remote.php/dav/` | An app password (Personal settings → Security → Devices & sessions) |
| Radicale | `https://radicale.example.com/` (or the URL your administrator gives you) | Your Radicale password |
| Baïkal, SOGo, other servers | The server's CalDAV root or principal URL | As configured by the server |

If discovery finds nothing, try the full calendar-home address the provider
documents, for example a Nextcloud user's
`https://cloud.example.com/remote.php/dav/calendars/USERNAME/`.

## Server differences

CalDAV servers differ in recurrence and scheduling support. OmaCalendar only
offers "this and following events" edits on a calendar after checking that the
server keeps them (**Check this-and-future support** in the event editor of a
recurring event).
See [compatibility](COMPATIBILITY.md) and [providers](PROVIDERS.md).
