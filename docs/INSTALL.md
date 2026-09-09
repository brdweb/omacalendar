# Install OmaCalendar

OmaCalendar 1.0.0 supports current **Arch/Omarchy on x86-64**. Install the
native Arch package; no custom package repository is required. A working
Linux Secret Service keyring is required for connected calendar accounts.

## Download and verify

Run in an empty directory. The GitHub CLI (`gh`) verifies the release provenance
and SPDX software inventory; `curl` downloads the files.

```bash
set -euo pipefail
version=1.0.0
package=omacalendar-1.0.0-1-x86_64.pkg.tar.zst
release_url="https://github.com/brdweb/omacalendar/releases/download/v${version}"
curl -fLO "${release_url}/${package}"
curl -fLO "${release_url}/SHA256SUMS"
grep " ${package}$" SHA256SUMS | sha256sum --check --strict
gh attestation verify "$package" --repo brdweb/omacalendar   --source-ref "refs/tags/v${version}"   --signer-workflow brdweb/omacalendar/.github/workflows/release.yml
gh attestation verify "$package" --repo brdweb/omacalendar   --source-ref "refs/tags/v${version}"   --signer-workflow brdweb/omacalendar/.github/workflows/release.yml   --predicate-type https://spdx.dev/Document/v2.3
sudo pacman -U "./${package}"
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
systemctl --user try-restart omacalendard.service
xdg-mime default org.omacalendar.OmaCalendar.desktop x-scheme-handler/omacalendar
```

Launch **OmaCalendar** from your application launcher or run `omacalendar`.
Connect calendars in **Accounts & settings**. Credentials are stored in your
keyring; event data is cached locally.

## Optional widget

Install the native app first, then follow the
[widget instructions](https://github.com/brdweb/omacalendar-widget#install-a-verified-release-archive).
The app can close while the widget continues using the background service.

## Update

Back up your data using [backup and recovery](BACKUP_AND_RECOVERY.md).
Close the app, download and verify the newer version's package, then repeat
the `pacman -U` and user-service commands above. Reopen the app to load the new
version. Packages downloaded from GitHub do not configure automatic updates.

## Remove

See [uninstall](UNINSTALL.md) for package removal and optional data cleanup.
Removing the package alone preserves calendar data and account credentials.
