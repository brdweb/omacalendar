# Getting started

OmaCalendar `1.0.0-beta.1` is a public-testing release for Omarchy Linux. Back up
important calendar data before testing it, and do not make the beta your only
copy of an important device-only calendar.

## Install the beta package from GitHub

The `v1.0.0-beta.1` GitHub release provides a native package for current
Omarchy on x86-64. In an empty directory, download and verify it before
installation:

```bash
set -euo pipefail
curl -fLO https://github.com/brdweb/omacalendar/releases/download/v1.0.0-beta.1/omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst
curl -fLO https://github.com/brdweb/omacalendar/releases/download/v1.0.0-beta.1/SHA256SUMS
grep ' omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst$' SHA256SUMS | sha256sum --check
gh attestation verify ./omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst \
  --repo brdweb/omacalendar \
  --source-ref refs/tags/v1.0.0-beta.1 \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml
gh attestation verify ./omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst \
  --repo brdweb/omacalendar \
  --source-ref refs/tags/v1.0.0-beta.1 \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml \
  --predicate-type https://spdx.dev/Document/v2.3
yay -U ./omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
systemctl --user try-restart omacalendard.service
```

The attestation commands require the optional GitHub CLI. This direct package
does not configure an update repository. Repeat the download, verification,
`yay -U` (or `pacman -U`), and user-service refresh steps for a newer release so an active
daemon does not continue running the replaced binary.

## Install from source

Clone the exact app release qualified with the current widget beta, install the
build dependencies, and compile the application:

```bash
set -euo pipefail
git clone --branch v1.0.0-beta.1 --depth 1 \
  https://github.com/brdweb/omacalendar.git
cd omacalendar
omarchy pkg add cmake ninja gcc qt6-base qt6-declarative \
  qt6-networkauth libical libsecret pkgconf
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
```

Google has approved OmaCalendar's OAuth branding and requested Calendar scopes.
A release package contains the public Desktop client configuration and requires
no credential setup by a tester. Source builds may instead use a separate
Google Desktop OAuth client as described in the test guide. The rest of the
application and widget can be evaluated without a Google account.

Start OmaCalendar from the Omarchy application launcher or run:

```bash
omacalendar
```

## Connect a calendar account

Open **Accounts & settings**, then choose a provider:

- **Google Calendar:** in a release package, select **Continue with Google in
  browser** and finish consent. Source builds must first provide their Google
  Desktop client configuration. Existing accounts may require one
  reauthorization after the requested scopes change.
- **CalDAV:** enter the server URL and credentials or app password supplied by
  the provider.
- **Local calendar:** create events without connecting a remote provider.

The first provider synchronization runs in the background. Cached calendars
remain usable while the desktop window is closed because the user-local daemon
is started on demand by its systemd socket.

## Everyday use

- Switch among Agenda, Day, Week, Month, and Year from the view selector.
- Use **New event** to create an event in the configured default calendar.
- Manage calendar visibility, ordering, colors, defaults, and deletions under
  **Accounts & settings**.
- Search from the toolbar. Invitation actions and provider synchronization are
  processed in the background.

The optional Omarchy widget is installed separately from the
[`omacalendar-widget`](https://github.com/brdweb/omacalendar-widget) repository.
It uses the same daemon and cached data but follows its own release versions.

## Data and troubleshooting

OmaCalendar stores its database under
`$XDG_DATA_HOME/omacalendar` (normally `~/.local/share/omacalendar`) and stores
provider credentials in Secret Service rather than SQLite. See:

- [Google test setup](GOOGLE_TESTING.md)
- [CalDAV test setup](CALDAV_TESTING.md)
- [Backup and recovery](BACKUP_AND_RECOVERY.md)
- [Compatibility and known limits](COMPATIBILITY.md)
- [Uninstall and data removal](UNINSTALL.md)

For a basic daemon check, run:

```bash
omacalendarctl system.info '{}'
```
