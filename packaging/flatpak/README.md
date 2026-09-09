# Flatpak release packaging

The GitHub release `.flatpak` file contains the desktop application, daemon,
CLI, libical 4.0.5, and libsecret 0.21.7 (`secret-tool`). It uses the KDE 6.10
runtime, which supplies Qt 6.10. The runtime is fetched from Flathub during
installation if necessary. OmaCalendar itself is distributed through GitHub;
this packaging does not imply a Flathub listing or an automatic app-update feed.
Bundled dependency translations stay inside the standalone app bundle rather
than requiring a separately published OmaCalendar Locale extension.

After verifying the release checksums and attestations:

```bash
flatpak install --user ./omacalendar-1.0.0-rc.3-linux-x86_64.flatpak
flatpak run org.omacalendar.OmaCalendar
```

Install each newer verified GitHub bundle with the same `flatpak install`
command. `flatpak update` updates the runtime but does not discover future
OmaCalendar bundles. A working desktop portal implementation and an unlocked
Secret Service keyring are needed for file pickers, browser authorization,
and provider credentials. Local calendars do not require provider credentials.

## Behavior and data

The Flatpak starts its own daemon when the app opens and stops that daemon when
the app exits. Synchronization and reminders therefore require the Flatpak
to remain running. Native packages use a systemd user service and socket
instead. To keep only the sandbox daemon running in a terminal:

```bash
flatpak run org.omacalendar.OmaCalendar --daemon
```

Stop that invocation with Ctrl+C. For a CLI request that starts a daemon if
necessary and stops it when the request completes:

```bash
flatpak run org.omacalendar.OmaCalendar --cli system.info
```

The Flatpak has a separate profile:

```text
~/.var/app/org.omacalendar.OmaCalendar/data/omacalendar/calendar.sqlite3
~/.var/app/org.omacalendar.OmaCalendar/config/omacalendar/
~/.var/app/org.omacalendar.OmaCalendar/cache/omacalendar/
$XDG_RUNTIME_DIR/app/org.omacalendar.OmaCalendar/omacalendar/daemon.sock
```

It does not read or migrate native application data. Native and Flatpak
instances can coexist without sharing a database, daemon socket, desktop
activation endpoint, or Secret Service credential namespace. Credentials use
the `application=org.omacalendar.OmaCalendar` keyring attribute. Native builds
retain `application=omacalendar`. If you import an existing profile, authorize
its provider accounts again; do not grant the Flatpak access to the native
database directory.

The Omarchy widget requires the native package and its daemon. It does not
automatically connect to the sandbox daemon. Choose the Arch/native install
when the widget or background reminders with the app closed are required.

File import, export, and browser launch use desktop portals. The manifest
grants network access, display/GPU access, and specific D-Bus access to Secret
Service and notifications. It grants no host/home filesystem access, no native
daemon socket access, and no unfiltered session/system bus access. The IPC
namespace permission supports X11 shared-memory rendering; it does not expose
the native OmaCalendar filesystem socket.

Uninstall the application with:

```bash
flatpak uninstall --user org.omacalendar.OmaCalendar
```

The profile remains available for reinstallation. After making any needed
backup, `flatpak uninstall --user --delete-data org.omacalendar.OmaCalendar`
also removes the Flatpak profile. Remove accounts in the application first to
clear their credentials, or use your desktop keyring manager to remove only
entries with the Flatpak application attribute above. Uninstalling the bundle
does not automatically erase provider credentials from Secret Service.

## Build and acceptance automation

Install `flatpak`, `flatpak-builder`, `ostree`, `jq`, `python3`, and ordinary shell tools. The build
downloads the KDE SDK/runtime and requires several GB of available disk space.
The source archive must be the exact release archive, with one top-level
directory. Library archives are pinned by version and SHA-256 in the manifest;
the runtime and SDK commits used are recorded alongside the bundle.

```bash
export FLATPAK_USER_DIR="$(mktemp -d /tmp/omacalendar-flatpak-install.XXXXXX)"
packaging/release/build-flatpak.sh \
  /path/to/omacalendar-1.0.0-rc.3-source.tar.gz 1.0.0-rc.3 /path/to/artifacts
packaging/release/verify-flatpak.sh \
  /path/to/artifacts/omacalendar-1.0.0-rc.3-linux-x86_64.flatpak 1.0.0-rc.3
```

Release builds require `OMACALENDAR_BUILD_GOOGLE_CLIENT_ID` and
`OMACALENDAR_BUILD_GOOGLE_CLIENT_SECRET` from protected CI configuration.
These public desktop OAuth client values are compiled using the same CMake
path as native packages. Flatpak Builder passes them through `secret-env`;
they are not rendered into the source manifest or command-line arguments.
Use `--without-google-config` only for local/PR builds that are not released.

`OMACALENDAR_BUILD_JOBS` controls parallelism (default 2), and
`OMACALENDAR_FLATPAK_WORK_DIR` optionally retains a specified build/cache
directory. Build outputs are:

- `omacalendar-VERSION-linux-x86_64.flatpak`: installable GitHub bundle.
- `omacalendar-VERSION-flatpak-runtime.txt`: source hash and runtime/SDK commit receipt.
- `libsecret-0.21.7.tar.xz`: the exact checksum-verified LGPL dependency source
  accompanying the bundle (the release also includes the shared libical source archive).
- `flatpak-stage/`: installed app files for generating a package-specific SPDX
  SBOM, checked out from the exact exported app ref rather than the larger
  Builder working tree; this directory is not a release asset.

After finalizing the SPDX document, verify it independently against the bundle:

```bash
python3 packaging/release/verify-flatpak-sbom.py \
  --bundle /path/to/artifacts/omacalendar-1.0.0-rc.3-linux-x86_64.flatpak \
  --sbom /path/to/artifacts/omacalendar-1.0.0-rc.3-flatpak.spdx.json \
  --version 1.0.0-rc.3
```

This imports only into a temporary OSTree repository, never an app installation,
and checks complete regular-file coverage, SHA-1/SHA-256 hashes, and application
and bundled-library package verification codes before attestations are issued.

The verifier requires a disposable `FLATPAK_USER_DIR` and refuses to replace an
existing app. It installs the bundle, exercises daemon/CLI IPC, creates a local
event, verifies persisted event/settings across complete sandbox restarts,
checks permissions and native-profile isolation, starts the actual desktop
with software/offscreen rendering, checks health after restart, and uninstalls
the bundle. Its temporary profile and logs are retained in the printed evidence
directory. The only extra filesystem permission during verification is that
temporary test profile.

Automated offscreen startup does not establish interactive portal, keyring,
Google OAuth, accessibility, or real Wayland compositor acceptance. Perform
those owner checks against the exact candidate before stable promotion.

References: [Flatpak single-file bundles](https://docs.flatpak.org/en/latest/single-file-bundles.html),
[sandbox permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html),
and [manifest/build options](https://docs.flatpak.org/en/latest/flatpak-builder-command-reference.html).
