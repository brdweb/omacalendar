# Omapak Flatpak handoff

Omapak is OmaCalendar's official end-user Flatpak channel. The application ID
is `org.omacalendar.OmaCalendar`. The native Arch package remains supported and
is released through this repository's signed-tag GitHub workflow; that workflow
builds and attests Arch artifacts only. A GitHub release asset is not an Omapak
Flatpak build.

## Install from Omapak

Use the Omapak remote and application ID directly:

```bash
flatpak remote-add --user --if-not-exists omapak https://repo.omapak.org/omapak.flatpakrepo
flatpak install --user omapak org.omacalendar.OmaCalendar
```

Do not ask users to download a Flatpak from the GitHub release page. The
Flatpak's catalog build, review, and publication happen independently in
Omapak.

## Why this directory has no manifest

The canonical public catalog recipe belongs in
[`outcrop-labs/omapak`](https://github.com/outcrop-labs/omapak), at
`apps/org.omacalendar.OmaCalendar/`. Omapak needs that recipe, its catalog
metadata, and its review history together to build the published application.
Keeping a second manifest here would let its source pin, runtime, dependencies,
or permissions drift from the Flatpak that users receive.

This repository deliberately owns only the Flatpak-specific source inputs:

- [`omacalendar-launcher`](omacalendar-launcher), the launcher selected by the
  catalog manifest as the Flatpak command;
- [`test-instance.sh`](test-instance.sh), the deterministic launcher regression
  fixture; and
- this maintenance guide plus the user and release documentation.

Do not add a manifest here. Make Flatpak recipe changes in the Omapak catalog
PR, pinned to a public OmaCalendar source commit.

## Launcher behavior and limitations

`packaging/flatpak/omacalendar-launcher` is executed only inside the
OmaCalendar Flatpak. It starts or attaches to the sandbox-local daemon through
`$XDG_RUNTIME_DIR/app/org.omacalendar.OmaCalendar/omacalendar/daemon.sock` and
uses file locks rather than PID checks, because separate Flatpak instances have
separate PID namespaces.

- The normal launcher path starts the GUI and keeps the daemon available while
  the launcher has clients to serve. It disables the GUI's own detached daemon
  autostart so the launcher owns the sandbox lifecycle.
- `flatpak run org.omacalendar.OmaCalendar --cli …` starts or attaches to that
  sandbox daemon for one CLI request.
- `flatpak run org.omacalendar.OmaCalendar --daemon` keeps a sandbox daemon
  running until that launcher is stopped.
- There is no native systemd user service or native daemon socket in the
  Flatpak. Background synchronization and reminders run only while a launcher
  remains running. Closing the owning launcher stops its daemon after attached
  clients finish.
- The Flatpak has an isolated profile under
  `~/.var/app/org.omacalendar.OmaCalendar/`; it does not read, migrate, or
  share the native profile, daemon endpoint, or credential namespace.
- The optional `org.omacalendar.widget` widget needs the native Arch install
  and native daemon. It cannot connect to the sandbox daemon, so the Flatpak
  has no native widget integration.

Choose the native Arch package when the widget or background reminders after
closing the application are required.

## Google OAuth in the catalog build

The Omapak build intentionally omits the protected native-release deployment
values `OMACALENDAR_BUILD_GOOGLE_CLIENT_ID` and
`OMACALENDAR_BUILD_GOOGLE_CLIENT_SECRET`. Do not copy those values, a GitHub
secret, or any cross-repository credential into the Omapak manifest, PR, or
workflow. Keep `OMACALENDAR_REQUIRE_GOOGLE_OAUTH_CONFIG` disabled for the
catalog build.

An Omapak user who wants Google Calendar imports their own Google desktop OAuth
credentials in **Accounts & settings**. This is expected behavior for this
credential-free Flatpak, not a catalog packaging failure.

## Updating the catalog recipe

Follow the complete dual-channel sequence in
[`docs/RELEASE.md`](../../docs/RELEASE.md). For the Omapak part, use this
ordered handoff:

1. Start after the signed OmaCalendar tag is public. Identify the tag's full
   target commit SHA; it is the only source revision the catalog manifest may
   use.
2. In a current fork of `outcrop-labs/omapak`, update
   `apps/org.omacalendar.OmaCalendar/`. Sync
   `packaging/org.omacalendar.OmaCalendar.metainfo.xml` into that app directory
   instead of maintaining an independent AppStream release history.
3. Pin the OmaCalendar source module to the full public SHA, not a branch,
   floating tag, or unpushed commit. Put the version, tag, and SHA in the PR
   description.
4. Review the catalog manifest's runtime and SDK and every dependency module.
   Update the runtime, dependency version, source URL, and immutable checksum
   when the tagged source needs it; keep every source reproducibly pinned.
   Ensure the manifest installs the source-owned launcher as its command.
5. Run the local launcher fixture before proposing the catalog change:

   ```bash
   packaging/flatpak/test-instance.sh
   ```

   It needs no arguments, installed Flatpak, GUI stack, or systemd service. It
   mocks the daemon, CLI, and GUI to exercise environment rejection, GUI and
   CLI attachment to one daemon, cleanup, and `--daemon` lifetime.
6. From the Omapak checkout, run the current Omapak preflight/judge against the
   catalog app directory and the exact source checkout:

   ```bash
   cargo run -p omapak-judge -- apps/org.omacalendar.OmaCalendar \
     --source-dir /absolute/path/to/omacalendar
   ```

7. Open the Omapak PR with the synchronized AppStream metadata, exact source
   pin, runtime/dependency changes, and preflight result. Monitor its public
   judge report, build status, and human review through catalog publication.
   Record the Omapak PR URL, source SHA, catalog commit, and final status in
   the matching `docs/releases/VERSION.md` acceptance record.

The source GitHub workflow must not create, update, or authenticate to the
Omapak PR. Its Arch release evidence and the Omapak catalog result are recorded
side by side, never substituted for one another.
