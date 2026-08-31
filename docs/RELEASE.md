# Release procedure

This procedure releases the OmaCalendar desktop application, daemon, and CLI.
The optional `org.omacalendar.widget` Quickshell plugin has an independent version,
qualification gate, tag, artifact set, and publication schedule in its own
repository. Automation creates draft app releases only; publication remains an
explicit maintainer action. Semantic prereleases such as `1.0.0-alpha` are
supported, but build metadata is not used in tags or artifact names.

## 1. Select the qualification gate

- Stable `1.0.0` requires every unchecked app exit criterion in
  [PLAN.md](PLAN.md), excluding the explicitly independent Phase 5 widget
  criteria, plus the complete app owner acceptance pass and no unresolved
  critical/high defect.
- `1.0.0-alpha` requires every non-negotiable pre-tag alpha gate in PLAN and
  [its acceptance record](releases/1.0.0-alpha.md). Post-tag rows must pass
  before the draft is published. Deferred stable matrices must be listed
  prominently as known limitations.
- Every public version requires the historical Google installed-app OAuth
  credential recorded in commit `2414615` to be confirmed revoked or rotated,
  plus a recorded repository-history decision. A scan allowlist does not close
  the incident.
- The security, privacy, support, website, and release-note claims must agree.

Alpha is for test or disposable calendars, carries no production support
promise, and does not complete any deferred stable gate. AUR publication is
deferred until stable 1.0.

## 2. Prepare app metadata

1. Set CMake's numeric project version and default suffix so their concatenation
   exactly matches the release version (`1.0.0` plus `-alpha` for this alpha).
2. Move changelog entries under a dated version heading, update AppStream and
   the compatibility table, and fill the acceptance record with platforms,
   toolchains, providers, results, and pre-tag approvals. The tag verifier
   derives the exact candidate commit from the signed tag target; record it
   with the post-tag draft evidence.
3. Run `packaging/release/test-versioning.sh`; it validates prerelease metadata
   without requiring a tag.
4. Qualify a clean committed checkout. A dirty-tree build is useful development
   evidence, but is never release evidence.

## 3. Create the app candidate

Create a signed annotated tag only from the accepted clean app commit:

```bash
git tag -s v1.0.0-alpha -m 'OmaCalendar 1.0.0-alpha'
packaging/release/verify-release.sh v1.0.0-alpha
git push origin v1.0.0-alpha
```

The local verifier cryptographically verifies the tag using configured Git
trust. The hosted workflows separately require GitHub to report each annotated
tag signature as verified.

Do not push a tag if local verification fails. Correct the candidate, remove
only an unpublished local tag, requalify, and create a new candidate. Never
move or replace a published tag.

The app tag workflow rebuilds/tests, stages and validates a `/usr` tree, creates
binary/source archives, records exact Arch packages, generates an SPDX JSON
SBOM, writes `SHA256SUMS`, and creates GitHub provenance/SBOM attestations. AUR
recipes are generated only for stable versions. It opens a **draft** release;
hyphenated versions are also explicitly marked **prerelease**. Widget automation
is separate and is not invoked or required by the app tag.

## 4. Verify artifacts independently

Download the draft app assets in a clean environment. For this alpha:

```bash
sha256sum --check SHA256SUMS
gh attestation verify omacalendar-1.0.0-alpha-linux-x86_64.tar.zst \
  --repo brdweb/omacalendar
gh attestation verify omacalendar-1.0.0-alpha-linux-x86_64.tar.zst \
  --repo brdweb/omacalendar \
  --predicate-type https://spdx.dev/Document/v2.3
```

Inspect the SBOM and install the exact archive on the release-reference VM.
Repeat the selected owner gate, including daemon restart, desktop launch,
offline display, one provider write round trip, and uninstall.

## 5. Publish

- Update the acceptance record with immutable commit IDs, draft URLs, artifact
  hashes, verification evidence, and completed external approvals.
- Publish the app draft. Keep `1.0.0-alpha` marked as a prerelease and include
  every known limitation and data-safety warning. Widget publication is an
  independent decision in the widget repository.
- For stable releases only, submit rendered source and binary PKGBUILDs after
  checksums match immutable assets and clean-chroot builds pass.
- Verify website/privacy/support links and monitor security/issue channels.

Never retag or replace a published asset. Fix a released mistake with a new
semantic version and changelog entry.
