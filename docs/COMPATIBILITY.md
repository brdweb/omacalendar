# Compatibility matrix

This matrix distinguishes intended support from completed release qualification.
Prerelease rows are for evaluation only; no entry is supported for production
until the stable 1.0 owner acceptance gate passes.

## Platform and toolchain

| Component | 1.0 target | Current qualification |
|---|---|---|
| Operating system | Current stable Omarchy, x86-64 | Local beta checks on Omarchy 4.0.2-1; exact-candidate clean-checkout gate pending |
| Qt | 6.8 or newer | Local 21-test matrix passes with Qt 6.11.2 |
| libical | 4.0 or newer | Current Omarchy/Arch package |
| GCC | Current Arch GCC | GCC 16.2.1 Release/Werror build and 21/21 tests pass; exact-candidate clean-checkout CI remains |
| Clang | Current Arch/LLVM Clang | Clang 22.1.8 Werror and ASan/UBSan builds pass; exact-candidate CI remains |
| systemd | User manager shipped by release-reference Omarchy | Staged `/usr` unit validation passes; VM gate remains |

Other Linux distributions and architectures may build from source but are not
part of the initial support promise.

## Providers

| Provider | Read | Write | Recurrence/guests | 1.0 status |
|---|---:|---:|---:|---|
| Device-only | Automated | Automated | Automated in part | Owner workflow pass pending |
| Google Calendar | Owner-tested discovery, sync, and restart | Fixture-tested; exact-candidate write round trip pending | Fixture-tested in part | Google verification approved; post-approval external-user matrix pending |
| Radicale 3.7.8 | Isolated pull/update/delete pass | Isolated timed/all-day/multi-day/recurring CRUD and offline drain pass | Detached occurrence and multiple alarms pass; guest/RSVP remains | Broad live slice passed; full matrix not qualified |
| Nextcloud | Target | Target | Target where supported | Pending live matrix |
| Fastmail CalDAV | Target | Target | Target where supported | Pending live matrix |
| HTTPS/webcal ICS | NASA public feed live-tested | Read-only | Import/export contract-tested | Sync/restart/manual refresh passed; authenticated/full matrix pending |

Provider capability discovery controls the UI. A server that lacks scheduling,
sync tokens, attendee writes, or another optional feature must show that limit
instead of accepting an operation it cannot preserve.

The Radicale and NASA results are development evidence, not complete provider
qualification. Live Google writes, Nextcloud, Fastmail, authenticated ICS, and
the untested portions of every provider matrix remain release blockers.

## App, IPC, database, and optional widgets

| App version | IPC major | Database schema | Widget compatibility | Status |
|---|---:|---:|---|---|
| `1.0.0-alpha` | 2 | 2 | Independent widget releases must declare IPC 2 and discover optional methods | Unsupported app prerelease |
| `1.0.0-beta.1` | 2 | 2 | Qualified widget releases declare IPC 2 and discover optional methods | Public-testing beta candidate |
| 1.0.x | 2 | 2 | Protocol/capability based; no widget version lock | Planned stable app line |

IPC major-version mismatches are rejected. Minor additions require capability
discovery. Widget versions, tags, and publication are independent of app
versions. Database migrations are forward-only; downgrade requires restoring a
backup created by the older build.

Each prerelease acceptance record captures the exact tested app versions. Before
stable 1.0, replace every remaining pending cell with exact Omarchy, Qt,
provider/server, app, IPC, and schema versions used in acceptance.
