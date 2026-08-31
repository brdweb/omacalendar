# Compatibility matrix

This matrix distinguishes intended support from completed release qualification.
The alpha row is for evaluation only; no entry is supported for production until
the stable 1.0 owner acceptance gate passes.

## Platform and toolchain

| Component | 1.0 target | Current qualification |
|---|---|---|
| Operating system | Current stable Omarchy, x86-64 | Local checks on Omarchy 4.0.1-1; pending clean-VM gate |
| Qt | 6.8 or newer | Local 20-test matrices pass with Qt 6.11.2; final release version remains open |
| GCC | Current Arch GCC | GCC 16.2.1 Release/Werror build and 20/20 tests pass; clean-checkout CI remains |
| Clang | Current Arch/LLVM Clang | Clang 22.1.8 Werror and ASan/UBSan builds each pass 20/20 tests |
| systemd | User manager shipped by release-reference Omarchy | Staged `/usr` unit validation passes; VM gate remains |

Other Linux distributions and architectures may build from source but are not
part of the initial support promise.

## Providers

| Provider | Read | Write | Recurrence/guests | 1.0 status |
|---|---:|---:|---:|---|
| Device-only | Automated | Automated | Automated in part | Owner workflow pass pending |
| Google Calendar | Owner-tested | Fixture-tested, not owner-accepted | Fixture-tested in part | Live full matrix pending |
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
| 1.0.x | 2 | 2 | Protocol/capability based; no widget version lock | Planned stable app line |

IPC major-version mismatches are rejected. Minor additions require capability
discovery. Widget versions, tags, and publication are independent of app
versions. Database migrations are forward-only; downgrade requires restoring a
backup created by the older build.

The alpha acceptance record captures the exact tested app versions. Before
stable 1.0, replace every remaining pending cell with exact Omarchy, Qt,
provider/server, app, IPC, and schema versions used in acceptance.
