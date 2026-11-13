# Backup and recovery

OmaCalendar caches provider data locally and will also contain device-only
calendars. Back up the data directory before installing a development build,
testing a schema migration, or removing an account.

## Storage locations

Default locations follow the XDG base-directory convention:

| Data | Default path |
|---|---|
| Database and migration archives | `$XDG_DATA_HOME/omacalendar/` or `~/.local/share/omacalendar/` |
| Configuration | `$XDG_CONFIG_HOME/omacalendar/` or `~/.config/omacalendar/` |
| Disposable cache | `$XDG_CACHE_HOME/omacalendar/` or `~/.cache/omacalendar/` |
| Runtime socket | `$XDG_RUNTIME_DIR/omacalendar/daemon.sock` |
| Provider credentials | Desktop Secret Service, not the database |

## Back up while running

The quickest backup needs no shutdown. Run

```bash
omacalendarctl backup
omacalendarctl backup --copy-to /media/usb/   # also copy it somewhere else
```

or open **Settings → Preferences** in the app and choose **Back up now** under
Support; the path of the new backup is shown below the button. Both ask
the running service to write a consistent snapshot of the database (SQLite
`VACUUM INTO`) to
`$XDG_DATA_HOME/omacalendar/backups/calendar-<UTC time>.sqlite3`
(`~/.local/share/omacalendar/backups/` by default). The `backups` directory is
mode `0700` and each backup is mode `0600`. The newest 5 backups are kept; older
ones are deleted when a new one is made. Copy a backup somewhere else (or use
`--copy-to`) if it should survive the loss of this disk.

A backup contains calendars, cached events, tasks, calendar sets, settings and
pending local changes. It does **not** contain passwords, OAuth tokens or the
Google client secret: those live only in the desktop keyring (Secret Service)
and are never copied into the database or the backup. They do not move to a new
machine with the backup. After restoring there, every Google and CalDAV account
must sign in again, and ICS subscriptions that use a username and password need
the password entered again. Device-only calendars need nothing.

To restore one of these backups:

```bash
systemctl --user stop omacalendard.socket omacalendard.service
data_root="${XDG_DATA_HOME:-$HOME/.local/share}/omacalendar"
mkdir -m 700 "$data_root/replaced-$(date +%Y%m%d-%H%M%S)"
mv "$data_root"/calendar.sqlite3* "$data_root"/replaced-*/
install -m 600 "$data_root/backups/calendar-<UTC time>.sqlite3" \
  "$data_root/calendar.sqlite3"
systemctl --user start omacalendard.socket
omacalendarctl system.health
```

The same rule as below applies: never restore a backup made by a newer version
into an older one.

## Create a consistent offline backup

To copy the whole data and configuration directories instead, close the desktop
and widget, then stop both the socket and daemon so another
client cannot reactivate SQLite while its database/WAL files are being copied:

```bash
systemctl --user stop omacalendard.socket omacalendard.service
trap 'systemctl --user start omacalendard.socket' EXIT
data_root="${XDG_DATA_HOME:-$HOME/.local/share}/omacalendar"
if [[ ! -d "$data_root" ]]; then
  echo "OmaCalendar data directory does not exist" >&2
  exit 1
fi
backup_root="$HOME/omacalendar-backup-$(date +%Y%m%d-%H%M%S)"
install -d -m 700 "$backup_root"
cp -a "$data_root" "$backup_root/data"
if [[ -d "${XDG_CONFIG_HOME:-$HOME/.config}/omacalendar" ]]; then
  cp -a "${XDG_CONFIG_HOME:-$HOME/.config}/omacalendar" "$backup_root/config"
fi
trap - EXIT
systemctl --user start omacalendard.socket
```

Protect the backup like calendar data. Secret Service credentials are not
included; on a new installation every account must sign in again.

## Restore

1. Close the app/widget and run
   `systemctl --user stop omacalendard.socket omacalendard.service` so socket
   activation cannot reopen the database during restoration.
2. Move the current data directory aside rather than deleting it.
3. Copy the backup's `data` directory back to the active XDG data path and its
   optional `config` directory to the active XDG config path.
4. Ensure every restored directory/file is owned by the current user and not
   accessible to group/other users.
5. Start `omacalendard.socket`, then run `omacalendarctl system.health` (or
   `system.info` on older development builds) before opening the clients.

Never restore a newer database into an older binary. Forward-only migrations do
not promise downgrade compatibility.

## Schema 1 transition

On first open, the schema 2 release checkpoints a detected schema 1 database and
copies the database plus any present `-wal` and `-shm` sidecars to a timestamped
`*.pre-v2-*.backup` file set. Every archive file is restricted to the current
user. It then initializes a clean schema 2 database at a staging path while the
schema 1 database remains at its original path. The active file is replaced only
after staging initialization succeeds.

The clean database contains only the device-local seed account and calendar;
remote accounts must be connected again. This transition does not delete or
otherwise modify legacy Secret Service items. They remain available until the
user explicitly removes them.

If archival or staging initialization fails, the schema 1 database remains at
the active path and the daemon reports startup failure. Keep it and every
timestamped archive for diagnosis. After correcting the reported filesystem or
SQLite error, retrying the same database is safe; do not delete the original or
its archive to force startup.

Before release, manually exercise the transition with a copy of a real schema 1
profile: stop the installed daemon, retain its database/WAL/SHM file set, record
the presence (not the value) of a test account's Secret Service item, and start
the schema 2 build. Confirm the archive files are mode `0600`, schema 2 requests
account reconnection, the legacy Secret Service item still exists, and an
explicit reconnect completes a provider sync. This live Secret Service/provider
check is an external release gate and is not performed by the unit tests.

## Provider resynchronization

A full sync is not a backup. Remote deletion can legitimately propagate to the
cache, and device-only calendars have no provider copy. If a provider cache is
damaged but remote data is authoritative, disconnect/rebuild only after making a
local backup and confirming which pending local operations would be discarded.
