# Command line

`omacalendarctl` talks to the OmaCalendar background service (`omacalendard`).
It ships with a man page (`man omacalendarctl`) and completions for bash, zsh
and fish.

## Everyday commands

```sh
omacalendarctl today                     # today's events
omacalendarctl agenda --days 14          # the next two weeks
omacalendarctl add "Lunch with Sam fri 12:30 @ Cafe"
omacalendarctl add "Trip to Denver Oct 3-6"   # no time: an all-day event
omacalendarctl sync                      # sync every account and wait
omacalendarctl backup                    # save a copy of the database
omacalendarctl backup --copy-to ~/Backups/
```

`add` understands the same text as Quick add in the app (dates, times,
durations such as `for 45 min`, repeats such as `every weekday`, and
`@ location`). It saves to your default calendar; pass `--calendar ID` to choose
another (`omacalendarctl calendars.list` shows the IDs).

`backup` writes a consistent copy of the calendar database to
`~/.local/share/omacalendar/backups/calendar-<UTC time>.sqlite3` (owner-only
permissions) and keeps the newest 5 there. `--copy-to PATH` also copies it to a
file or into a directory you choose; an existing file is never overwritten. The
backup does not contain passwords or sign-in tokens, which stay in your keyring;
see [BACKUP_AND_RECOVERY.md](BACKUP_AND_RECOVERY.md).

Add `--json` to any of these to get machine-readable output.

## Raw IPC calls

Any method from [IPC.md](IPC.md) can be called directly. The response is one
line of JSON on stdout; an error is printed as JSON on stderr with exit status 1.

```sh
omacalendarctl methods                   # what this service supports
omacalendarctl system.info
omacalendarctl events.search '{"query":"dentist"}'
omacalendarctl --watch system.subscribe '{"topics":["events","reminders"]}'
```

### Passwords and secrets

Never put a password or client secret on the command line: other local users
can read it from the process list, and your shell saves it in its history.
Give the parameters on stdin instead, with `-` (or `--params-stdin`):

```sh
read -rs password
printf '{"serverUrl":"https://dav.example.com/","username":"me","password":"%s"}' \
  "$password" | omacalendarctl accounts.addCalDav -
```

## Options

| Option | Meaning |
|---|---|
| `--timeout SECONDS` | How long to wait for a response (default 5, or 120 for `sync` and `backup`) |
| `--watch` | After the response, print notifications on stdout until interrupted |
| `--json` | JSON output for `today`, `agenda`, `add`, `sync`, `backup` and `methods` |
| `--days N` | Days for `agenda` (default 7) |
| `--calendar ID` | Calendar for `add` |
| `--copy-to PATH` | For `backup`: also copy the backup to this file or directory |
| `-`, `--params-stdin` | Read JSON parameters from stdin |
| `--version` | Print the version |

If the service is not running, start it with
`systemctl --user start omacalendard`.
