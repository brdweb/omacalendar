# Install, update, and remove OmaCalendar

The preparation candidate is `1.0.0-rc.2`. It is a GitHub **draft**, available
to authenticated repository collaborators with write access for owner testing. The public beta
remains available separately. Do not rename or substitute beta assets for the
candidate. [Owner testing](OWNER_TESTING.md) records the remaining stable gates.

## Download and verify

Use an empty download directory and GitHub CLI authenticated with repository
write access. Download the selected package, checksums, and documentation from the
same draft (omit the unused package patterns):

```bash
gh release download v1.0.0-rc.2 --repo brdweb/omacalendar \
  --pattern 'omacalendar-1.0.0rc2-1-x86_64.pkg.tar.zst' \
  --pattern 'omacalendar_1.0.0~rc.2-1_amd64.deb' \
  --pattern 'omacalendar-1.0.0-rc.2-linux-x86_64.flatpak' \
  --pattern SHA256SUMS --pattern '*documentation.tar.gz'
```

For each selected install package, set `package` to its exact filename and verify:

```bash
set -euo pipefail
package=omacalendar-1.0.0rc2-1-x86_64.pkg.tar.zst
awk -v file="$package" '$2 == file { print; count++ } END { if (count != 1) exit 1 }' \
  SHA256SUMS | sha256sum --check --strict
gh attestation verify "./$package" --repo brdweb/omacalendar \
  --source-ref refs/tags/v1.0.0-rc.2 \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml
gh attestation verify "./$package" --repo brdweb/omacalendar \
  --source-ref refs/tags/v1.0.0-rc.2 \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml \
  --predicate-type https://spdx.dev/Document/v2.3
```

Stop if any command fails. Each native/sandbox package has its own relevant
SBOM. The documentation archive and source archive belong to the same commit.
The SBOM describes the shipped app, files and bundled libraries; it is not an
inventory of the user's operating system or the separately installed KDE
runtime. The release also includes native build dependency inventories and
the Flatpak runtime/SDK commit receipt. Original source archives accompany the
privately bundled libical and libsecret libraries.
The documentation archive has provenance, not a package SBOM; verify it with:

```bash
set -euo pipefail
documentation=omacalendar-1.0.0-rc.2-documentation.tar.gz
awk -v file="$documentation" '$2 == file { print; count++ } END { if (count != 1) exit 1 }' \
  SHA256SUMS | sha256sum --check --strict
gh attestation verify "./$documentation" --repo brdweb/omacalendar \
  --source-ref refs/tags/v1.0.0-rc.2 \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml
tar -xzf "$documentation"
```

After a release is public, GitHub's release page also supports browser downloads;
draft assets require authentication and ordinary public download URLs will fail.

## Arch and Omarchy, x86-64

Use a fully updated current Arch/Omarchy installation. Back up your existing
profile, then install the verified native package:

```bash
sudo pacman -U ./omacalendar-1.0.0rc2-1-x86_64.pkg.tar.zst
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
systemctl --user try-restart omacalendard.service
omacalendarctl system.info '{}'
omacalendar
```

`yay -U` can replace `sudo pacman -U`. Current Arch/Omarchy dependencies are
resolved by pacman; no AUR registration is needed. The native app includes the
daemon, CLI, desktop integration and widget activation helper. Install the
widget independently using the
[widget RC installation and test guide](https://github.com/brdweb/omacalendar-widget/blob/v0.1.0-rc.2/docs/STABLE_ACCEPTANCE.md).

The user socket keeps the backend available while the desktop is closed. The
first CLI/widget connection starts the daemon. After package upgrades, restart
the existing service as above so it runs the replaced binary.

## Ubuntu 26.04, amd64 (.deb)

This Debian-format package is compiled for Ubuntu 26.04 with Qt 6.10. It includes
a private libical 4 and does not replace the distribution's system libical.
It does not support Debian 12/13 or Ubuntu 24.04: their Qt versions lack APIs
used by the app. Use Flatpak on a system with older Qt libraries.

```bash
sudo apt install ./omacalendar_1.0.0~rc.2-1_amd64.deb
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
systemctl --user try-restart omacalendard.service
omacalendarctl system.info '{}'
omacalendar
```

Install in a graphical user session with a working Secret Service keyring
(for example GNOME Keyring or a compatible desktop service), a browser, and
notifications. Unlock the keyring before connecting accounts. System package
dependencies do not create or unlock a user keyring automatically.

## Flatpak, x86-64

Install Flatpak using your distribution's package manager. Add Flathub to obtain
the KDE runtime, then install the verified GitHub bundle:

```bash
flatpak remote-add --user --if-not-exists flathub \
  https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./omacalendar-1.0.0-rc.2-linux-x86_64.flatpak
flatpak run org.omacalendar.OmaCalendar
```

This is a GitHub bundle, not a Flathub application listing. The bundle points to
Flathub for its KDE 6.10 runtime. It needs network access for providers and the
OAuth loopback callback, display/GPU access, and narrowly named desktop services
for keyring storage and notifications. It does not need full home-directory or
host execution access. Use the desktop file picker for import/export.

Flatpak keeps data under
`~/.var/app/org.omacalendar.OmaCalendar/data/omacalendar/`, configuration under
the corresponding `config/`, and its IPC socket under
`$XDG_RUNTIME_DIR/app/org.omacalendar.OmaCalendar/omacalendar/`. Native and
Flatpak profiles are separate; connecting an account in one does not sign into
the other. The native Quickshell widget requires the native app/daemon and does
not connect to this sandbox. Do not manually share their databases or sockets.

The Flatpak launcher manages its sandbox backend. Background synchronization
and reminders require that Flatpak process to remain running; native systemd
socket activation is not installed by the bundle. Quit/stop and reopen the
Flatpak to load an updated bundle.

## First run and daily use

Release packages include the public Google Desktop OAuth client configuration;
select **Accounts & settings → Continue with Google in browser**. CalDAV uses
the provider's HTTPS URL and password/app password. Device-only calendars need
no account. Read-only ICS subscriptions do not permit event editing.

For a Flatpak status check, run:

```bash
flatpak run org.omacalendar.OmaCalendar --cli system.info
```

Select a default calendar, create a synthetic event, and verify it appears in
Agenda, Day, Week, Month and Year. Use search, calendar visibility and keyboard
navigation as described in [Getting started](GETTING_STARTED.md). Provider
sync runs in the background; errors and blocked operations remain visible.

## Updates and recovery

These downloads do not configure an app update repository. Download and verify
the newer release, then repeat its package install command. `flatpak update`
updates the runtime but a GitHub bundle requires installing the newer bundle.
Arch prerelease `1.0.0rc2` and Debian `1.0.0~rc.2-1` sort below stable `1.0.0`.
Use [backup and recovery](BACKUP_AND_RECOVERY.md) before upgrades. Database
downgrades require restoration of a backup made by the older version.

## Removal

For a native package, close the app and stop/disable its backend first:

```bash
systemctl --user disable --now omacalendard.socket
systemctl --user stop omacalendard.service
# Arch/Omarchy:
sudo pacman -R omacalendar
# Ubuntu (run only on Ubuntu):
sudo apt remove omacalendar
systemctl --user daemon-reload
```

Run only the package-manager command for your distribution. For Flatpak:

```bash
flatpak kill org.omacalendar.OmaCalendar
flatpak uninstall --user org.omacalendar.OmaCalendar
```

If the Flatpak is already stopped, `flatpak kill` may report that it is not
running; proceed with uninstall. Normal removal retains user data. Deliberate
profile/credential removal and provider disconnect are covered in
[Uninstall](UNINSTALL.md). Removing the widget alone must preserve app data.
