# Debian release package

The supported native `.deb` target is **Ubuntu 26.04 LTS, amd64**. Debian 12,
Debian 13, and Ubuntu 24.04 have older Qt versions and must use the Flatpak distribution.
Compatibility with other Debian-derived distributions is not implied by the
`.deb` extension; Qt packages can require distribution-specific ABI versions.

The application is compiled against Ubuntu's Qt 6.10 and system libraries. Ubuntu
26.04 ships libical 3, so the build downloads a checksum-pinned libical 4.0.5 source
release and installs its shared library privately in `/usr/lib/omacalendar`.
The executable RUNPATH selects it without replacing the system libical. The
unmodified libical source tarball is also an output for redistribution alongside
the package, and its license notices are included in the installed documentation.
The package's shared-library dependencies are calculated with `dpkg-shlibdeps`;
the Qt QML modules, SQLite plugin, Wayland plugin, and desktop services needed at
runtime are explicit dependencies.

## Build and verify

From the repository root, build the isolated toolchain:

```bash
docker build -f packaging/debian/Dockerfile -t omacalendar-debian-builder .
mkdir -p artifacts-debian
docker run --rm \
  -v "$PWD:/source:ro" -v "$PWD/artifacts-debian:/output" \
  -e SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)" \
  omacalendar-debian-builder \
  bash packaging/release/build-deb-release.sh 1.0.0-rc.3 /output
```

Replace the example version with the exact version being built. Canonical SemVer
prereleases retain Debian's `~` ordering in the package's internal `Version`
field: `1.0.0-rc.3` becomes `1.0.0~rc.3-1`. The download filename is separately
`omacalendar_1.0.0-rc.3-1_amd64.deb`, using GitHub-safe characters because
GitHub normalizes tildes in asset names. Stable `1.0.0` still produces
`omacalendar_1.0.0-1_amd64.deb` with internal version `1.0.0-1`. Package-manager
upgrades compare internal versions, not filenames. The build runs the automated suite and
checks the staged install, builds the package twice and compares the results.
It writes an Ubuntu dependency inventory and the libical source archive next to
the package. Fixed timestamps make package assembly reproducible from the same
staged binaries; rebuilding the compiler toolchain from moving apt mirrors is
not claimed to be bit-for-bit reproducible.

Official release automation supplies `OMACALENDAR_BUILD_GOOGLE_CLIENT_ID` and
`OMACALENDAR_BUILD_GOOGLE_CLIENT_SECRET` through protected CI environment
configuration and sets `OMACALENDAR_REQUIRE_GOOGLE_OAUTH_CONFIG=ON`.
Credential-free developer builds default to `OFF`. Do not put credentials in
command arguments, source archives, logs, or a Docker image.

Validate installation in a fresh runtime container with no development packages:

```bash
docker run --rm \
  -v "$PWD:/source:ro" -v "$PWD/artifacts-debian:/packages:ro" \
  ubuntu:26.04 \
  bash /source/packaging/release/test-deb-package.sh \
    1.0.0-rc.3 /packages/omacalendar_1.0.0-rc.3-1_amd64.deb
```

This installs with apt, verifies dependencies and package contents, runs the
installed daemon restart/IPC smoke test and desktop startup smoke at scales
1, 1.25, and 2, then removes the package and verifies its files are gone. A
container has no graphical login session; this does not establish successful
Secret Service unlock, live provider authentication, desktop notifications, or
systemd user activation in a complete desktop session.

## Install, update, and remove

Follow the [candidate download and verification guide](https://github.com/brdweb/omacalendar/blob/v1.0.0-rc.3/docs/INSTALL.md)
to obtain the `.deb` and `SHA256SUMS` from the same GitHub release. In the download
directory, verify the checksum for the exact filename, then install it:

```bash
set -euo pipefail
grep ' omacalendar_1.0.0-rc.3-1_amd64.deb$' SHA256SUMS | sha256sum --check
sudo apt install ./omacalendar_1.0.0-rc.3-1_amd64.deb
systemctl --user daemon-reload
systemctl --user enable --now omacalendard.socket
systemctl --user try-restart omacalendard.service
omacalendar
```

A desktop Secret Service implementation must be running and unlocked before
adding provider credentials. GNOME Keyring is recommended on desktops that do
not already supply one. On KDE, enable the wallet's Secret Service integration.
Download and install a newer release `.deb` in the same way to update; installing
this file does not add an apt repository or automatic update channel. Back up
your calendar profile before upgrading as described in
[the backup and recovery guide](https://github.com/brdweb/omacalendar/blob/v1.0.0-rc.3/docs/BACKUP_AND_RECOVERY.md).

```bash
systemctl --user disable --now omacalendard.socket
systemctl --user stop omacalendard.service
sudo apt remove omacalendar
systemctl --user daemon-reload
```

Removing or purging the package retains user calendar data and stored provider
credentials. See [the uninstall guide](https://github.com/brdweb/omacalendar/blob/v1.0.0-rc.3/docs/UNINSTALL.md) for explicit
profile and credential removal. Full application documentation is installed
under `/usr/share/doc/OmaCalendar/docs/`; the Debian package and bundled library
license notices are under `/usr/share/doc/omacalendar/`.
