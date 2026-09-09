# Stable release preparation

The target is OmaCalendar `1.0.0` and an independently qualified widget
`0.1.0`. The owner authorized committing the tested changes and continuing
through qualification and release on 2026-09-09. The app replacement candidate
is `1.0.0-rc.4`; the independent widget candidate remains `0.1.0-rc.3`.
Neither version is a declaration that stable gates pass.

RC4 follows the [expanded acceptance failures](ACCEPTANCE_2026-09-09.md) in
RC3: recurrence dates/identity, CalDAV acknowledgement, conflict chronology,
local date conversion and accessible event naming. It also closes the
first-use recurrence capability and cached-floating upgrade paths before
qualification. Prior signed tags and draft artifacts remain unchanged.
Use the [RC4 record](releases/1.0.0-rc.4.md); earlier preparation or development
evidence does not automatically qualify the new downloaded artifacts.

## Delivery plan

1. Reconcile both clean repositories and their published beta evidence.
2. Add native Debian-family and sandboxed Flatpak builds alongside the Arch
   package. Build every format from the same app commit, with the protected
   public Google Desktop client configuration supplied by CI.
3. Compare downloaded bundle/package regular-file inventories and hashes with
   their SPDX documents, and verify GitHub asset names against the checksum and
   install instructions. Validate installation, launch, daemon restart, local
   data persistence, removal, and the existing compiler, sanitizer, QML,
   provider-contract, security-regression, and performance checks.
4. Prepare the widget candidate independently, run its portable and Omarchy
   integration suites, and record its IPC compatibility with the app.
5. Publish source changes and prepare GitHub draft candidate releases with
   checksums, package-specific SBOMs, install/update/uninstall documentation,
   and a focused owner checklist available together.
6. Record results against the exact downloaded artifacts. Fix any
   observed defect, repeat affected qualification, and publish stable versions
   only after every applicable criterion in `PLAN.md` and the widget's own
   acceptance record passes.

## Evidence rules

Automated contract tests, live provider tests, package installation, and
hands-on desktop acceptance are separate evidence. A prior beta exception
does not approve an unperformed stable check. Hosted build success does not
prove a clean Omarchy desktop installation or Google/Fastmail account behavior.
Unperformed or blocked checks remain pending with a named next action.

`PLAN.md` remains the stable scope and gate authority. The beta records remain
historical. Candidate evidence and the owner checklist are recorded alongside
the candidate under `docs/releases/`; final artifact hashes and hosted workflow
links identify exactly what was tested.

## Distribution scope

- Arch: native x86-64 package for current Arch/Omarchy, including user systemd
  socket activation and the native widget integration.
- Debian family: an independently compiled native amd64 package, with the
  precise supported distribution baseline recorded after a clean build and
  install test. An Arch binary repackaged as a `.deb` is not acceptable.
- Flatpak: an installable x86-64 bundle, with its runtime obtained from
  Flathub. A GitHub bundle does not imply a Flathub listing. Document its
  sandbox permissions, data paths, backend lifecycle, and widget compatibility.

No AUR or marketplace listing approval is inferred from GitHub publication.
