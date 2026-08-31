# Security policy

## Supported versions

`1.0.0-alpha` is an unsupported evaluation prerelease. Security fixes may be
issued while it is evaluated, but it is not suitable for production or as the
sole copy of calendar data. Stable support begins with 1.0; after that, only the
latest minor release line receives security updates unless the compatibility
matrix states otherwise.

## Report a vulnerability

Do not open a public issue for a suspected vulnerability, exposed credential,
or report containing private calendar data.

Use GitHub's **Report a vulnerability** form in the repository Security tab to
send a private report. Include:

- the affected commit or version;
- operating system and relevant provider;
- a minimal reproduction using synthetic data;
- expected and observed impact; and
- any suggested mitigation.

Remove access/refresh tokens, passwords, cookie values, event text, attendee
addresses, and private server URLs. If a real credential was exposed, revoke or
rotate it before reporting; repository maintainers cannot revoke provider
credentials for you.

The maintainer will acknowledge a usable report within seven days, coordinate
validation and a fix privately, and credit the reporter if requested. Timing of
disclosure depends on severity, provider coordination, and the availability of
a tested fix.

## Security boundary

The daemon is the only component intended to access provider credentials,
remote services, or the writable database. Desktop and widget processes use a
user-local IPC protocol. A local attacker already able to execute arbitrary code
as the same Unix user is outside the primary isolation boundary, although file,
socket, log, and IPC permissions must still minimize accidental exposure.

The current [threat model](docs/THREAT_MODEL.md) documents assets, trust
boundaries, required controls, and known residual risk.
