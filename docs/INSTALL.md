# Install, update, and remove OmaCalendar

OmaCalendar supports one binary distribution: the native x86-64 Arch package
for current Arch Linux and Omarchy. This keeps the desktop application, user
systemd daemon, command-line client, and optional Omarchy widget on one profile
and one lifecycle.

The qualified replacement candidate is `1.0.0-rc.5`. Its draft also contains
older experimental package formats produced before the Arch-only decision.
Those files remain immutable release evidence and are not supported install
options. Future candidates and stable releases produce only the Arch package.

## Download and verify

Use an empty directory. Draft downloads require an authenticated GitHub CLI;
public releases can also be downloaded in a browser.

```bash
set -euo pipefail
gh release download v1.0.0-rc.5 --repo brdweb/omacalendar \
  --pattern 'omacalendar-1.0.0rc5-1-x86_64.pkg.tar.zst' \
  --pattern SHA256SUMS --pattern '*documentation.tar.gz'

package=omacalendar-1.0.0rc5-1-x86_64.pkg.tar.zst
awk -v file="$package" '$2 == file { print; count++ } END { if (count != 1) exit 1 }' \
  SHA256SUMS | sha256sum --check --strict
gh attestation verify "./$package" --repo brdweb/omacalendar \
  --source-ref refs/tags/v1.0.0-rc.5 \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml
gh attestation verify "./$package" --repo brdweb/omacalendar \
  --source-ref refs/tags/v1.0.0-rc.5 \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml \
  --predicate-type https://spdx.dev/Document/v2.3
```

Stop if verification fails. The package SBOM covers the exact staged app
payload; the Arch dependency inventory records the build environment. Source
and documentation archives are supplied separately.

## Install on Arch or Omarchy

Back up the existing profile, fully update the system, and install the verified
package:

```bash
sudo pacman -U ./omacalendar-1.0.0rc5-1-x86_64.pkg.tar.zst
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
systemctl --user try-restart omacalendard.service
omacalendarctl system.info '{}'
omacalendar
```

`yay -U` can replace `sudo pacman -U`. Pacman resolves runtime dependencies;
no custom repository or AUR registration is required. The package includes the
app, daemon, CLI, desktop integration, and widget activation helper. Install
the optional widget independently using its
[RC installation and test guide](https://github.com/brdweb/omacalendar-widget/blob/v0.1.0-rc.4/docs/STABLE_ACCEPTANCE.md).

The user socket keeps the backend available while the desktop is closed. The
first desktop, CLI, or widget connection starts it on demand.

## First run and daily use

Release packages include the public Google Desktop OAuth client configuration.
Choose **Accounts & settings → Continue with Google in browser**. CalDAV uses
the provider's HTTPS URL and password or app password. Device-only calendars
need no account, and ICS subscriptions are read-only.

Choose a default calendar, create a synthetic event, and confirm it in Agenda,
Day, Week, Month, and Year. Provider sync runs through the native daemon while
the desktop is closed.

## Update

Direct GitHub downloads do not configure an update repository. Download and
verify the next release, then install it with `pacman -U` and restart the active
daemon so it loads the new binary:

```bash
sudo pacman -U ./omacalendar-NEW_VERSION-1-x86_64.pkg.tar.zst
systemctl --user daemon-reload
systemctl --user try-restart omacalendard.service
omacalendarctl system.info '{}'
```

Use [backup and recovery](BACKUP_AND_RECOVERY.md) before upgrading. Database
downgrades require restoring a backup created by the older version.

## Remove

Normal package removal retains user data:

```bash
systemctl --user disable --now omacalendard.socket
systemctl --user stop omacalendard.service
sudo pacman -R omacalendar
systemctl --user daemon-reload
```

Provider disconnect, deliberate profile removal, and widget restoration are
covered in [Uninstall](UNINSTALL.md).
