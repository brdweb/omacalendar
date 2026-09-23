# Install OmaCalendar

OmaCalendar 1.1.1 supports the Omapak Flatpak as the recommended official
installation route and the verified native package as a supported alternative
for current **Arch/Omarchy on x86-64**. Connected calendar accounts need a
working Linux Secret Service keyring.

## Recommended: Omapak Flatpak

On a Linux system with Flatpak, add the Omapak remote and install OmaCalendar:

```bash
flatpak remote-add --user --if-not-exists omapak https://repo.omapak.org/omapak.flatpakrepo
flatpak install --user omapak org.omacalendar.OmaCalendar
```

### Run, update, and remove

```bash
flatpak run org.omacalendar.OmaCalendar
flatpak update --user org.omacalendar.OmaCalendar
flatpak uninstall --user org.omacalendar.OmaCalendar
```

The normal uninstall command keeps the Flatpak profile. See
[Uninstall](UNINSTALL.md) to delete that profile deliberately.

### Profile, background behavior, and Google Calendar

The Flatpak uses its own profile under
`~/.var/app/org.omacalendar.OmaCalendar/` and its own Secret Service credential
namespace, `application=org.omacalendar.OmaCalendar`. It neither reads nor
migrates the native profile or its `application=omacalendar` credentials.

The Flatpak has no native widget integration. Its sandbox daemon is managed by
the Flatpak launcher, so synchronization and reminders run in the background
only while the graphical app or another launcher invocation remains running.
To keep that launcher running without the desktop window:

```bash
flatpak run org.omacalendar.OmaCalendar --daemon
```

Stop that foreground command with Ctrl+C. The native package instead uses a
socket-activated user service and is required for the optional widget.

Omapak intentionally omits protected Google OAuth deployment values. To connect
Google Calendar, create your own Google Desktop OAuth credentials and import
the credentials JSON from **Accounts & settings** before authorizing the
account.

## Supported native alternative: verified GitHub Arch package

Run in an empty directory. The GitHub CLI (`gh`) verifies the release provenance
and SPDX software inventory; `curl` downloads the files.

```bash
set -euo pipefail
version=1.1.1
package=omacalendar-1.1.1-1-x86_64.pkg.tar.zst
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

After native installation, launch **OmaCalendar** from your application launcher
or run `omacalendar`. Connect calendars in **Accounts & settings**. Credentials
are stored in the native `application=omacalendar` keyring namespace; event
data is cached locally.

## Native-only optional widget

Install the native app first, then follow the
[widget instructions](https://github.com/brdweb/omacalendar-widget#install-a-verified-release-archive).
The widget requires the native socket-activated service and does not connect to
the Omapak sandbox daemon. The native app can close while the widget continues
using the background service.

## Update the native Arch package

Back up your data using [backup and recovery](BACKUP_AND_RECOVERY.md).
Close the native app, download and verify the newer GitHub package, then repeat
the `pacman -U` and user-service commands above. Reopen the app to load the new
version. Packages downloaded from GitHub do not configure automatic updates.

## Remove either installation

See [uninstall](UNINSTALL.md) for Flatpak and native package removal and
optional data cleanup. Normal package removal preserves calendar data and
account credentials; the Flatpak and native installations use separate
profiles and Secret Service namespaces.
