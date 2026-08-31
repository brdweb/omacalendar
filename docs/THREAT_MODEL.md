# Threat model

## Assets and trust boundaries

Protected assets are provider credentials, private event/attendee content,
device-only calendars, pending mutations, reminder text, and the Omarchy config
backups used during widget activation.

Trust boundaries exist between:

- remote Google, CalDAV, and ICS endpoints and `omacalendard`;
- the daemon and its user-local IPC clients;
- SQLite/Secret Service and application processes;
- untrusted event/ICS content and QML/notifications/logging; and
- the widget activation helper and user-owned Omarchy/Hyprland configuration.

The primary attacker is remote content or a remote endpoint attempting data
exfiltration, memory/resource exhaustion, credential redirection, malformed
parser input, or unintended local actions. Cross-user local access and
accidental disclosure through permissions/logs are in scope. An attacker already
able to execute arbitrary code as the same Unix user is not treated as isolated
by the desktop session, but credentials should still never be needlessly
exposed.

## Required controls

| Threat | Required mitigation |
|---|---|
| Credential theft through storage/logs | Secret Service only; stdin credential handoff; structured redaction; fixture scanning |
| Cross-user IPC/database access | User-only directories/socket/database, bounded framing, protocol/version validation |
| Credential leak on redirect | TLS verification and same-origin credential forwarding only |
| Malicious ICS/XML/JSON | Bounded responses/depth/fields, parser errors without content reflection, no shell or HTML execution |
| Mutation replay/duplication | Stable client IDs, dependencies, leases, remote reconciliation, transactional acknowledgement |
| Silent remote overwrite | Expected revisions and explicit keep-local/remote/merge conflicts |
| Notification/lock-screen disclosure | Full/title/generic privacy modes and safe default preview |
| Widget privilege expansion | Thin IPC client; no credentials/database/provider network access |
| Config corruption during activation | Exact backup, validation before commit, atomic replacement, full rollback, ownership checks |
| Supply-chain substitution | Checksums, SPDX SBOM, dependency review, signed GitHub artifact attestations |
| Resource exhaustion | Message/response/query bounds, paginated sync, bounded concurrency and recurrence expansion |

## Security verification

- Unit/fuzz-style malformed input tests for IPC, JSON, XML, and ICS boundaries.
- Provider contract tests for redirects, auth failures, oversized responses,
  throttling, conflicts, and lost acknowledgments.
- Permission checks for every XDG artifact and the runtime socket.
- ASan/UBSan CI and dependency/secret review.
- Isolated activation/rollback tests that prove unrelated user configuration is
  unchanged.
- Manual diagnostic-export and notification privacy review before release.

## Residual risk

Calendar providers see data sent to them and enforce their own retention and
sharing rules. Desktop notification servers, accessibility services, backups,
and same-user processes may observe displayed local content. Generic CalDAV/ICS
implementations vary; unsupported semantics must be surfaced, but perfect
lossless behavior cannot be claimed until a provider passes the acceptance
matrix. These limitations belong in release notes and the compatibility table.
