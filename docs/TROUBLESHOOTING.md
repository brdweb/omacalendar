# Troubleshooting

## The calendar service is not running

The app and `omacalendarctl` talk to the background service `omacalendard`,
which systemd starts on demand. If the app shows **Reconnect** or the CLI says
it cannot connect:

```sh
systemctl --user status omacalendard.socket omacalendard.service
systemctl --user restart omacalendard.socket
journalctl --user -u omacalendard -n 50
```

## Passwords and sign-ins (the keyring)

OmaCalendar never stores passwords or Google tokens in its database. It keeps
them in your desktop keyring through the freedesktop **Secret Service**, using
the `secret-tool` command from `libsecret`. If the keyring is missing or locked,
connecting an account fails and existing accounts ask you to sign in again.

### Check that a keyring answers

```sh
pacman -Q libsecret            # provides secret-tool
secret-tool search --all application omacalendar
```

- `secret-tool: command not found`: install `libsecret`.
- The command waits for a password prompt or fails with "Cannot autolaunch
  D-Bus" or "The name org.freedesktop.secrets was not provided": no keyring
  service is running in your session. See below.
- It prints nothing: the keyring works but holds no OmaCalendar items yet,
  which is normal before you connect an account.

### GNOME Keyring (Omarchy's default)

Omarchy uses `gnome-keyring`. It is normally unlocked when you log in through
the display manager. If you log in another way, or changed your login password,
the "login" keyring may stay locked:

```sh
pacman -Q gnome-keyring
busctl --user list | grep org.freedesktop.secrets
```

If no `org.freedesktop.secrets` name is listed, start the daemon for this
session and make sure your session starts it in future:

```sh
gnome-keyring-daemon --start --components=secrets
```

If it is listed but every request asks for a password, unlock or recreate the
"login" keyring with **Seahorse** (`pacman -S seahorse`). A keyring whose
password no longer matches your login password must be unlocked by hand after
each login until you change its password to match.

### KeePassXC

KeePassXC can act as the Secret Service instead of GNOME Keyring. Enable
**Settings → Secret Service Integration**, choose which database group exposes
secrets, and keep the database unlocked while OmaCalendar syncs. Only one
Secret Service provider can own the bus name; stop `gnome-keyring` secrets if
KeePassXC should handle them.

### After moving to a new machine

Keyring items are not part of an OmaCalendar backup. After restoring a backup
on another machine, each account shows **Sign in**; sign in again (Google) or
re-enter the password (CalDAV, password-protected feeds).

## A provider keeps failing

The error banner names the problem and offers the next step (sign in, open
account settings, or retry). **Details** shows the provider's original message.
Common causes:

| Message | What to check |
|---|---|
| Wrong user name or password | CalDAV providers such as iCloud and Fastmail need an app-specific password, not your account password |
| Server not found | The server address; see [CalDAV setup](CALDAV_SETUP.md) |
| Secure connection failed | The server's TLS certificate; OmaCalendar does not accept invalid certificates |
| Sign-in expired | Choose **Sign in**; see [Google sign-in](GOOGLE_SIGN_IN.md) |

Activity lists queued changes and conflicts per account.

## Reporting a problem

`omacalendarctl system.health` and **Preview diagnostics** (Accounts & settings →
Preferences) produce a summary
without event content. Remove account addresses and calendar URLs before
attaching anything to an issue.
