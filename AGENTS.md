# AGENTS.md

Instructions for coding agents in this repository (Cursor cloud agents, Codex, Claude Code). Who reviews and who merges is in Review and merge policy.

Personal or machine-specific notes never go in tracked files. Claude Code auto-loads the gitignored `CLAUDE.local.md`; Codex users put personal guidance in `~/.codex/AGENTS.md`; `AGENTS.local.md` is a gitignored scratch file that tools don't load automatically.

## Project

OmaCalendar (`omacalendar`, v2.0.2, MIT) is a local-first Qt 6.9 / C++20 calendar for current Arch and Omarchy. The supported installable binary is the native x86-64 Arch package. It ships `omacalendar` (Qt Quick desktop), `omacalendard` (daemon), `omacalendarctl` (CLI), and `scripts/omacalendar-widgetctl`. The daemon owns the SQLite database, provider network access, Secret Service credentials, reminders, and the user-local IPC server. The desktop app and the separate Omarchy widget consume presentation data over IPC. Providers are Google Calendar, CalDAV, local calendars, and read-only ICS.

- `src/app/` — Qt Quick desktop UI (`omacalendar`).
- `src/daemon/` — `omacalendard`.
- `src/cli/` — `omacalendarctl`.
- `src/core/` — domain, SQLite schema and migrations, Secret Service, recurrence.
- `src/ipc/` — local IPC protocol.
- `src/providers/` — Google, CalDAV, and ICS. The local provider lives in `src/sync/`.
- `src/sync/` and `src/reminders/` — sync coordinator and reminder scheduling.
- `tests/` — Qt Test binaries and `test_daemon_contract.py`, registered with `ctest`. `scripts/tests/test_widgetctl.py` is a `ctest` target too.
- `packaging/` — Arch package, desktop and AppStream metadata, and `packaging/release/` checks.
- `docs/` — `ARCHITECTURE.md`, `IPC.md`, `RELEASE.md`, `PRIVACY.md`, `THREAT_MODEL.md`, and the other guides.
- `.github/workflows/` — `ci.yml`, `secret-scan.yml`, `dependency-review.yml`, `release.yml`, `pages.yml`.

## Commands

Pull requests run four checks. Run the local commands below as written. The baseline job uses the `archlinux:base-devel` image and installs `appstream`, `cmake`, `desktop-file-utils`, `git`, `libical`, `libsecret`, `ninja`, `pkgconf`, `qt6-base`, `qt6-declarative`, `qt6-networkauth`, and `systemd`. The metadata job installs `appstream`, `clang`, `desktop-file-utils`, `git`, and `python`. README.md's Debug configure is a local starting point. The pull-request baseline is the Release command below.

**Source and package metadata** on every pull request (`.github/workflows/ci.yml`):

```sh
mapfile -t sources < <(git ls-files '*.cpp' '*.h')
clang-format --dry-run --Werror "${sources[@]}"
for script in packaging/arch/*.in packaging/release/*.sh; do
  bash -n "${script}"
done
packaging/release/test-versioning.sh
packaging/release/test-arch-package.sh
python3 packaging/release/test-finalize-sbom.py
desktop-file-validate packaging/org.omacalendar.OmaCalendar.desktop
appstreamcli validate --no-net --strict packaging/org.omacalendar.OmaCalendar.metainfo.xml
```

CI creates an unprivileged user and runs the two `packaging/release/test-*.sh` scripts with `runuser`. `test-arch-package.sh` exits when `EUID` is 0, so run them as a normal user. `test-finalize-sbom.py` is a `unittest` module.

**Current Arch package baseline** on every pull request. The build, test, smoke, and QML lint step sets `QT_QPA_PLATFORM=offscreen`:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DOMACALENDAR_ENABLE_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
packaging/release/smoke-daemon.sh build
packaging/release/smoke-app.sh build
packaging/release/check-qmllint.sh build
DESTDIR="$PWD/stage" cmake --install build
packaging/release/verify-install.sh "$PWD/stage"
cp -a stage stage-uninstall
packaging/release/verify-uninstall.sh \
  "$PWD/stage-uninstall" "$PWD/build/install_manifest.txt"
```

**Gitleaks** (`.github/workflows/secret-scan.yml`) and **Dependency review** (`.github/workflows/dependency-review.yml`) are GitHub Actions. Their workflows invoke no shell command, so there is no local command to copy. Gitleaks scans full history with `.gitleaks.toml`. Dependency review runs on pull requests and fails on severity `moderate` or higher.

The `build` (Clang RelWithDebInfo) and `sanitizers` (ASan and UBSan) jobs in `ci.yml` run when the event is `schedule`, or `workflow_dispatch` with `run_extended` set to true. Tagging, signing, and draft creation belong to `.github/workflows/release.yml` and `docs/RELEASE.md`.

## Definition of done

- Start from a GitHub Issue that states the goal, non-goals, acceptance criteria, and how to prove them.
- The Source and package metadata commands and the Current Arch package baseline commands pass locally. Paste that output in the PR body. Gitleaks and Dependency review have no local command; say so. If another command could not be run, name it and why.
- Every bug fix adds a regression test that fails before the fix.
- A change visible in the Qt Quick UI includes a screenshot.
- The PR links the Issue and states what was not done.

## Boundaries

- Open draft pull requests only. Never push to `main`.
- Never force-push a shared branch, delete branches or tags, or create tags or releases.
- Agents never tag, sign, or publish. A release needs a GPG-signed annotated tag. CI verifies that signature. The release workflow creates a draft, and the maintainer publishes it. See `docs/RELEASE.md`.
- No major dependency bumps. No edits to `.github/`, release config, auth, database migrations, or secrets, unless the Issue asks.
- Never weaken or delete a test to make CI pass.
- Keep a pull request to about 400 changed lines. Split work that would exceed that.
- When a requirement is ambiguous, ask in the PR instead of guessing.

## Commits and PRs

Use Conventional Commits: `feat:`, `fix:`, `docs:`, `refactor:`, `test:`, `build(deps):`, `chore:`. The PR title uses the same form, because squash merges take the title as the commit subject. Never add `Co-Authored-By` or any other AI co-author or attribution trailer to a commit message or a PR body.

## Review guidance

Codex (`chatgpt-codex-connector`) is the only automatic pull-request reviewer. Claude Code is optional: someone calls it in with an `@claude` comment, where configured, for risky pull requests only, and it never runs automatically. Reviews should focus on daemon ownership of SQLite, Secret Service credentials, provider network access, and reminders; IPC clients that only consume presentation DTOs; all-day dates kept as dates and timed events keeping their original IANA zones; unknown provider data preserved on edit; durable idempotent outbox writes and explicit conflict, offline, and auth states; one-way schema migrations; and Arch package, desktop, and AppStream metadata. The agent that opened the PR fixes review findings.

## Review and merge policy

- Bots never post GitHub approvals.
- A PR merges only after CI is green and every Codex review thread is fixed and resolved.
- The triage bot may auto-merge only patch and minor dependency bumps, security fixes, docs, tests, and small fixes under about 50 changed lines.
- Other bots and agents merge only work the maintainer has explicitly approved.
- Everything else waits for the maintainer: major version bumps, auth, payments, database migrations, CI or release configuration, secrets, and large or ambiguous PRs.
- Squash merge. Commits carry no `Co-Authored-By` or other AI co-author trailers.

## Learning

Read [`docs/agent-mistakes.md`](docs/agent-mistakes.md) before starting. When a reviewer catches a repeatable mistake, add an entry to that file.
