# Contributing to OmaCalendar

OmaCalendar is pre-release software. Discuss substantial changes in an issue
before investing in implementation, and keep each pull request focused on one
behavioral change.

## Development setup

Install the dependencies listed in the [README](README.md), then configure a
separate build directory:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DOMACALENDAR_ENABLE_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Before submitting a change, also run:

```bash
clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.h')
packaging/release/check-qmllint.sh build
```

Use `-DOMACALENDAR_ENABLE_SANITIZERS=ON` for a local ASan/UBSan build. Do not run
live-provider tests against a personal or production calendar; use isolated test
accounts and synthetic events.

## Design and implementation rules

- Keep provider networking, credentials, writable storage, and reminders in the
  daemon. Desktop and widget code consume presentation DTOs over IPC.
- Treat all-day values as dates and retain original IANA zones for timed events.
- Preserve unknown provider data when editing a resource.
- Queue provider writes durably and make retries idempotent.
- Expose unsupported, offline, retry, blocked, auth, and conflict states instead
  of silently dropping data or applying last-write-wins.
- Update the IPC documentation and protocol tests in the same pull request as a
  wire-contract change.
- Add automated coverage for every bug fix and meaningful new behavior.

## Privacy and fixtures

Never commit OAuth clients intended to remain private, tokens, passwords,
cookies, real attendee addresses, private calendar URLs, or raw personal ICS
files. Sanitize recorded fixtures and inspect the complete diff before pushing.
Logs and diagnostics must use local IDs and status codes rather than event text
or credential material.

## Pull-request checklist

- The change is inside the documented 1.0 scope or updates that scope explicitly.
- GCC/Clang build, tests, formatting, and warning-free QML lint pass.
- New user-visible behavior includes empty, offline, failure, and accessibility
  states where applicable.
- Documentation and changelog are updated.
- No generated build output or secret material is included.
- Provider-affecting changes include mock contract tests; live acceptance results
  are recorded separately without credentials.

By contributing, you agree that your contribution is licensed under the
repository's [MIT License](LICENSE).
