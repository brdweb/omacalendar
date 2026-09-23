# Release procedure

OmaCalendar supports two install paths: the native x86-64 Arch package for
current Arch/Omarchy and the Omapak Flatpak
(`org.omacalendar.OmaCalendar`). The signed-tag GitHub workflow builds,
attests, and drafts only the native Arch release. Its source and documentation
archives, checksums, SPDX SBOM, and attestations are Arch-channel evidence;
they are not Flatpak artifacts. Omapak builds the Flatpak independently from
its catalog manifest.

The optional `org.omacalendar.widget` Quickshell plugin has its own repository,
version, qualification gates, tag, and publication schedule. It consumes the
native daemon installed by the Arch package.

## 1. Close the qualification gates

The stable release requires every applicable app criterion in [PLAN.md](PLAN.md),
[release validation](OWNER_TESTING.md), and no unresolved critical or
high defect. Provider, desktop, widget, security, and exact-package evidence
remain separate; one kind of pass does not imply another.

Credential incidents must be resolved before publication. Release notes, support scope, privacy
claims, screenshots, and package metadata must agree with shipped behavior.

## 2. Prepare metadata

1. Set the CMake numeric version and optional prerelease suffix.
2. Add a dated changelog and AppStream entry.
3. Update the compatibility table and create the exact acceptance record under
   `docs/releases/`.
4. Run `packaging/release/test-versioning.sh` and the full clean CI matrix.

A dirty-tree build is development evidence and cannot qualify a release.

### Waiving a gate

A gate the maintainer consciously decides to skip is recorded as
`WAIVED - reason`, never as an unearned `PASS`. Its evidence column must still
state what actually ran. `verify-release.sh` accepts a waiver in place of a
pass, so a patch release can be fast-tracked without the full matrix, and the
acceptance record keeps saying which qualification the release did not receive.
Waive deliberately: the skipped checks are the ones that catch what review and
the unit suites do not.

## 3. Rehearse the Arch release

Run the **Release candidate** workflow manually with the intended semantic
version. It executes the protected Google Desktop client injection, build,
tests, staged install/uninstall checks, Arch package assembly, SPDX completion,
and artifact upload without creating a tag or GitHub release.

Download the rehearsal artifact and perform the applicable owner checks on the
release-reference Omarchy system. Rehearsal artifacts have no release
attestations and cannot be published as the final package.

## 4. Create the signed native candidate

Create a signed annotated tag from the accepted clean commit:

```bash
git tag -s vVERSION -m 'OmaCalendar VERSION'
packaging/release/verify-release.sh vVERSION
git push origin vVERSION
```

For an RC draft, use its exact version and
`verify-release.sh --draft-candidate vVERSION-rc.N`. This validates draft-only
authorization without granting stable acceptance.

Never move a pushed tag or replace assets on a published release. Correct an
unpublished local tag in place only before it leaves the workstation; correct
a pushed candidate with a new semantic version.

The signed-tag workflow verifies GitHub's tag signature result, repeats the
build and tests, builds the Arch package, completes the exact payload SBOM,
attests the source archive/package/documentation, writes one `SHA256SUMS`, and
creates a draft release. Automation never publishes the draft.

## 5. Verify downloaded Arch artifacts

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

## 6. Prepare and submit the separate Omapak catalog PR

Omapak is the official Flatpak channel, but it is an independent build and
review path. Its canonical recipe lives in
[`outcrop-labs/omapak`](https://github.com/outcrop-labs/omapak), under
`apps/org.omacalendar.OmaCalendar/`. This repository intentionally carries no
second manifest: it owns the launcher, its regression fixture, and the
maintenance and user documentation only. Do not upload or attach a Flatpak to
the GitHub release, and do not treat GitHub release attestations as evidence
for an Omapak build.

After the signed tag is pushed and its target source commit is public:

1. Work from a current fork of `outcrop-labs/omapak`, not from this repository.
   Update the catalog app directory, including its manifest and
   `metadata.yml`.
2. Sync the current
   `packaging/org.omacalendar.OmaCalendar.metainfo.xml` into the catalog app
   directory. Copy the source AppStream metadata rather than maintaining a
   divergent release history; its app ID and release entry must match the
   tagged source.
3. Pin the catalog manifest's OmaCalendar source module to the full, exact
   public commit targeted by the signed tag. Do not use a branch, a floating
   tag, or an unpublished commit. State the version, tag, and full source SHA
   in the Omapak PR description.
4. Review the catalog manifest's runtime and SDK and every dependency module
   for the tagged source. Update dependency versions, source URLs, and
   immutable checksums where required, and keep all source pins reproducible.
   The manifest must install
   `packaging/flatpak/omacalendar-launcher` as the Flatpak command; see
   [the launcher handoff](../packaging/flatpak/README.md).
5. The Omapak build intentionally omits the protected
   `OMACALENDAR_BUILD_GOOGLE_CLIENT_ID` and
   `OMACALENDAR_BUILD_GOOGLE_CLIENT_SECRET` deployment values. Keep
   `OMACALENDAR_REQUIRE_GOOGLE_OAUTH_CONFIG` disabled, do not copy values or
   cross-repository credentials into the manifest or workflow, and tell users
   to import their own Google desktop credentials in **Accounts & settings**.
6. Run the source-owned local launcher fixture
   (`packaging/flatpak/test-instance.sh`) before submitting. It is a
   deterministic, no-argument regression check for the GUI, `--cli`, and
   `--daemon` launcher paths; it does not require an installed Flatpak.
7. From the Omapak checkout, run its current preflight/judge against the
   catalog app directory and this exact source checkout:

   ```bash
   cargo run -p omapak-judge -- apps/org.omacalendar.OmaCalendar \
     --source-dir /absolute/path/to/omacalendar
   ```

8. Open the Omapak PR only with the exact source pin, synchronized AppStream
   metadata, dependency/runtime changes, and preflight result. Record the PR
   URL, catalog commit, tagged source SHA, and result in the matching
   `docs/releases/VERSION.md` acceptance record as soon as it is opened.

## 7. Publish and monitor each channel

Publish the GitHub draft only after the Arch metadata and owner/provider gates
are complete. Verify the Arch release page, checksum, package, documentation,
privacy, support, screenshot, and issue links from an anonymous session.
GitHub packages do not configure automatic pacman updates; users download and
verify each newer package explicitly.

Monitor the Omapak PR separately through its preflight, public judge report,
human review, merge, and catalog publication. Record the final PR URL and
catalog state beside the Arch workflow URLs; a published GitHub release neither
creates nor proves the Omapak build. Monitor GitHub issues, Omapak feedback,
OAuth status, and provider regressions. Release corrections under a new
semantic version and preserve the old release and evidence.
