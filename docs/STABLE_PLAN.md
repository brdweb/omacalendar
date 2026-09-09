# Stable release preparation

The target is OmaCalendar `1.0.0` and an independently qualified widget
`0.1.0`. The owner requested preparation on 2026-09-08 and will perform the
hands-on acceptance pass on 2026-09-09. Candidate versions are
`1.0.0-rc.1` and `0.1.0-rc.1`; neither is a declaration that stable gates pass.

## Delivery plan

1. Reconcile both clean repositories and their published beta evidence.
2. Add native Debian-family and sandboxed Flatpak builds alongside the Arch
   package. Build every format from the same app commit, with the protected
   public Google Desktop client configuration supplied by CI.
3. Validate package contents, installation, launch, daemon restart, local
   data persistence, removal, and the existing compiler, sanitizer, QML,
   provider-contract, security-regression, and performance checks.
4. Prepare the widget candidate independently, run its portable and Omarchy
   integration suites, and record its IPC compatibility with the app.
5. Publish source changes and prepare GitHub draft candidate releases with
   checksums, package-specific SBOMs, install/update/uninstall documentation,
   and a focused owner checklist available together.
6. Record tomorrow's results against the exact downloaded artifacts. Fix any
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
