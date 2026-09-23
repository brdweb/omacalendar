# Uninstall and data removal

Uninstalling binaries and deleting calendar data are separate, deliberate
operations. Make a backup first if device-only events or pending offline changes
may be needed later.

For setup instructions, see [Install](INSTALL.md).

## Omapak Flatpak

Close OmaCalendar, or stop a foreground
`flatpak run org.omacalendar.OmaCalendar --daemon` process with Ctrl+C. Remove
the Flatpak while keeping its isolated profile:

```bash
flatpak uninstall --user org.omacalendar.OmaCalendar
```

After making any needed backup, remove the Flatpak and its profile deliberately:

```bash
flatpak uninstall --user --delete-data org.omacalendar.OmaCalendar
```

`--delete-data` removes only the Flatpak profile:

- `~/.var/app/org.omacalendar.OmaCalendar/data/omacalendar`;
- `~/.var/app/org.omacalendar.OmaCalendar/config/omacalendar`; and
- `~/.var/app/org.omacalendar.OmaCalendar/cache/omacalendar`.

It does not remove Flatpak credentials from Secret Service. After provider
access has been revoked, remove only the Flatpak credential namespace with:

```bash
secret-tool clear application org.omacalendar.OmaCalendar
```

## Native Arch package

Close the native app/widget and stop the daemon:

```bash
systemctl --user disable --now omacalendard.socket
systemctl --user stop omacalendard.service
```

Remove the native package through pacman:

```bash
sudo pacman -R omacalendar
```

For a manual CMake installation, remove only the paths recorded by that build's
`install_manifest.txt`; do not recursively delete `/usr` or `/usr/local`.

Reload desktop/systemd metadata after native package removal:

```bash
systemctl --user daemon-reload
update-desktop-database "${XDG_DATA_HOME:-$HOME/.local/share}/applications" 2>/dev/null || true
```

## Restore the native Omarchy clock

If the native Quickshell widget was activated, run the OmaCalendar restore
command provided by the independently installed widget release before removing
either native component. It must restore the backed-up clock placement, center
anchor, and calendar shortcut and remove only OmaCalendar-owned configuration.
Do not manually delete the backup until the restored shell configuration has
validated and restarted.

## Optional native local-data deletion

The following native-profile data is not removed automatically:

- `$XDG_DATA_HOME/omacalendar` (database and migration backups);
- `$XDG_CONFIG_HOME/omacalendar`;
- `$XDG_CACHE_HOME/omacalendar`; and
- native OmaCalendar items in Secret Service (`application=omacalendar`).

Review these paths and backup contents before deleting them. To remove all
native OmaCalendar Secret Service items after provider access has been revoked:

```bash
secret-tool clear application omacalendar
```

Also revoke OmaCalendar in the provider's security settings and remove any
provider-specific app password. Local deletion cannot remove provider-side
calendar data or revoke a token on an unreachable provider. If both the
Flatpak and native app were installed, remove each profile and its credential
namespace separately; neither removal affects the other.
