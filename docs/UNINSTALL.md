# Uninstall and data removal

Uninstalling binaries and deleting calendar data are separate, deliberate
operations. Make a backup first if device-only events or pending offline changes
may be needed later.

## Remove the package

Close the app/widget and stop the daemon:

```bash
systemctl --user disable --now omacalendard.socket
systemctl --user stop omacalendard.service
```

For an Arch package, remove the installed package through pacman/AUR tooling.
For a manual CMake installation, remove only the paths recorded by that build's
`install_manifest.txt`; do not recursively delete `/usr` or `/usr/local`.

Reload desktop/systemd metadata after package removal:

```bash
systemctl --user daemon-reload
update-desktop-database "${XDG_DATA_HOME:-$HOME/.local/share}/applications" 2>/dev/null || true
```

## Restore the Omarchy clock

If the Quickshell widget was activated, run the OmaCalendar restore command
provided by the independently installed widget release before removing either
component. It must restore the backed-up clock placement, center anchor, and
calendar shortcut and remove only OmaCalendar-owned configuration. Do not
manually delete the backup until the restored shell configuration has validated
and restarted.

## Optional local-data deletion

The following data is not removed automatically:

- `$XDG_DATA_HOME/omacalendar` (database and migration backups);
- `$XDG_CONFIG_HOME/omacalendar`;
- `$XDG_CACHE_HOME/omacalendar`; and
- OmaCalendar items in Secret Service.

Review these paths and backup contents before deleting them. To remove all
OmaCalendar Secret Service items after provider access has been revoked:

```bash
secret-tool clear application omacalendar
```

Also revoke OmaCalendar in the provider's security settings and remove any
provider-specific app password. Local deletion cannot remove provider-side
calendar data or revoke a token on an unreachable provider.
