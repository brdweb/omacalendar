# Release procedure

OmaCalendar produces and supports one installable binary: the native x86-64
Arch package for current Arch/Omarchy. Source and documentation archives,
checksums, an SPDX SBOM, and attestations accompany it. The release workflow
does not produce a generic binary archive, Debian package, Flatpak bundle, or
AUR recipe.

RC5 predates this scope decision. Preserve its signed tag, draft, assets, and
qualification records unchanged. Future candidates and stable releases follow
this procedure.

The optional `org.omacalendar.widget` Quickshell plugin has its own repository,
version, qualification gates, tag, and publication schedule. It consumes the
native daemon installed by the Arch package.

## 1. Close the qualification gates

Stable `1.0.0` requires every applicable app criterion in [PLAN.md](PLAN.md),
the complete [owner checklist](OWNER_TESTING.md), and no unresolved critical or
high defect. Provider, desktop, widget, security, and exact-package evidence
remain separate; one kind of pass does not imply another.

Every public version also requires the historical Google installed-app OAuth
credential recorded in commit `2414615` to remain revoked and the repository
history decision to remain documented. Release notes, support scope, privacy
claims, screenshots, and package metadata must agree with shipped behavior.

## 2. Prepare metadata

1. Set the CMake numeric version and optional prerelease suffix.
2. Add a dated changelog and AppStream entry.
3. Update the compatibility table and create the exact acceptance record under
   `docs/releases/`.
4. Run `packaging/release/test-versioning.sh` and the full clean CI matrix.

A dirty-tree build is development evidence and cannot qualify a release.

## 3. Rehearse the Arch release

Run the **Release candidate** workflow manually with the intended semantic
version. It executes the protected Google Desktop client injection, build,
tests, staged install/uninstall checks, Arch package assembly, SPDX completion,
and artifact upload without creating a tag or GitHub release.

Download the rehearsal artifact and perform the applicable owner checks on the
release-reference Omarchy system. Rehearsal artifacts have no release
attestations and cannot be published as the final package.

## 4. Create the signed candidate

Create a signed annotated tag from the accepted clean commit:

```bash
git tag -s v1.0.0 -m 'OmaCalendar 1.0.0'
packaging/release/verify-release.sh v1.0.0
git push origin v1.0.0
```

For an RC draft, use its exact version and
`verify-release.sh --draft-candidate v1.0.0-rc.N`. This validates draft-only
authorization without granting stable acceptance.

Never move a pushed tag or replace assets on a published release. Correct an
unpublished local tag in place only before it leaves the workstation; correct
a pushed candidate with a new semantic version.

The signed-tag workflow verifies GitHub's tag signature result, repeats the
build and tests, builds the Arch package, completes the exact payload SBOM,
attests the source archive/package/documentation, writes one `SHA256SUMS`, and
creates a draft release. Automation never publishes the draft.

## 5. Verify downloaded artifacts

Download every draft asset into an empty directory. Require the actual hosted
filenames and bytes to match the manifest, then verify both provenance and SPDX
attestations for the Arch package:

```bash
sha256sum --check --strict SHA256SUMS
gh attestation verify ./omacalendar-VERSION-1-x86_64.pkg.tar.zst \
  --repo brdweb/omacalendar \
  --source-ref refs/tags/vVERSION \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml
gh attestation verify ./omacalendar-VERSION-1-x86_64.pkg.tar.zst \
  --repo brdweb/omacalendar \
  --source-ref refs/tags/vVERSION \
  --signer-workflow brdweb/omacalendar/.github/workflows/release.yml \
  --predicate-type https://spdx.dev/Document/v2.3
```

Inspect the package and SBOM, install the exact download on clean current
Omarchy, verify package integrity, socket activation, desktop launch, provider
continuity, backup/upgrade behavior, and normal removal. Record the package
hash, signed tag object, source commit, and workflow URLs in the acceptance
record.

## 6. Publish and monitor

Publish only after strict metadata verification passes and every applicable
owner/provider gate is complete. Verify the release page, checksum, package,
documentation, privacy, support, screenshot, and issue links from an anonymous
session.

GitHub packages do not configure automatic pacman updates. Users download and
verify each newer package explicitly. Monitor GitHub issues, OAuth status, and
provider regressions. Release corrections under a new semantic version and
preserve the old release and evidence.
