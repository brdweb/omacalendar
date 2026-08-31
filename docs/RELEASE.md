# Release procedure

This procedure releases the OmaCalendar desktop application, daemon, and CLI.
The optional `org.omacalendar.widget` Quickshell plugin has an independent
version, qualification gate, tag, artifact set, and publication schedule in its
own repository. Automation creates draft app releases only; publication remains
an explicit maintainer action. Semantic prereleases such as `1.0.0-beta.1` are
supported; build metadata is not used in tags or artifact names.

## 1. Select and record the qualification gate

- Stable `1.0.0` requires every unchecked app exit criterion in
  [PLAN.md](PLAN.md), excluding the independent widget criteria, plus the full
  owner acceptance pass and no unresolved critical/high defect.
- `1.0.0-beta.1` requires every gate in [BETA_PLAN.md](BETA_PLAN.md) and its
  [acceptance record](releases/1.0.0-beta.1.md). `PENDING` in a pre-tag or
  external-approval row blocks tagging; `PENDING` in a post-tag row blocks
  publication.
- The historical `1.0.0-alpha` evidence remains in
  [its immutable acceptance record](releases/1.0.0-alpha.md).
- Every public version requires the historical Google installed-app OAuth
  credential recorded in commit `2414615` to remain revoked, with the
  repository-history decision recorded. A scan allowlist alone does not close
  that incident.
- Security, privacy, support, website, screenshots, package metadata, and
  release-note claims must agree with the shipped behavior.

Prereleases carry no production support promise and must not be the sole copy
of important device-only calendar data.

## 2. Prepare app metadata

1. Set CMake's numeric project version and default suffix so their concatenation
   exactly matches the release version (`1.0.0` plus `-beta.1` for the first
   beta).
2. Move changelog entries under a dated version heading; update AppStream and
   the compatibility table; and fill the acceptance record with exact
   platforms, toolchains, providers, results, candidate commit, and approvals.
3. Run `packaging/release/test-versioning.sh`. It validates semantic versioning,
   release metadata, prerelease-aware AUR rendering, and the fact that pending
   acceptance evidence cannot pass strict tag verification.
4. Qualify a clean committed checkout. A dirty-tree build is development
   evidence, never release evidence.

## 3. Create the app candidate

Create a signed annotated tag only from the accepted clean app commit:

```bash
git tag -s v1.0.0-beta.1 -m 'OmaCalendar 1.0.0-beta.1'
packaging/release/verify-release.sh v1.0.0-beta.1
git push origin v1.0.0-beta.1
```

The local verifier cryptographically verifies the tag using configured Git
trust. The hosted workflow separately requires GitHub to report the annotated
tag signature as verified.

Do not push a tag if verification fails. Correct the candidate, delete only an
unpublished local tag, requalify, and create a new candidate version if needed.
Never move or replace a published tag.

The tag workflow rebuilds/tests, stages and validates a `/usr` tree, creates
binary/source archives and a native Arch package, records exact build packages,
generates an SPDX JSON SBOM, writes `SHA256SUMS`, renders future source and
binary AUR recipes, and creates GitHub provenance/SBOM attestations. It opens a
**draft** release and marks hyphenated versions as prereleases. Widget
automation is separate.

## 4. Verify draft artifacts independently

Download the draft assets in a clean environment:

```bash
sha256sum --check SHA256SUMS
gh attestation verify omacalendar-1.0.0-beta.1-linux-x86_64.tar.zst \
  --repo brdweb/omacalendar
gh attestation verify omacalendar-1.0.0-beta.1-linux-x86_64.tar.zst \
  --repo brdweb/omacalendar \
  --predicate-type https://spdx.dev/Document/v2.3
```

Inspect the SBOM and install the exact archive on the release-reference Omarchy
profile. Repeat daemon restart, desktop launch, offline cached display, one
provider write round trip, backup/upgrade continuity, and uninstall. Record
immutable hashes and workflow URLs in the acceptance record.

## 5. Publish the GitHub beta

- Confirm the acceptance record identifies the tag target and contains no
  credentials, private endpoints, or real calendar content.
- Review every known limitation and the generated release notes.
- Publish the draft with the prerelease flag intact.
- Verify the homepage, privacy, support, screenshot, download, and issue links.

## 6. Publish the native Arch package on GitHub

The first beta ships an installable Arch package directly in the GitHub release:

```text
omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst
```

It is built from the same staged `/usr` tree already exercised by the release
workflow, included in `SHA256SUMS`, and covered by GitHub provenance. Download
and verify the package before installing it:

```bash
sha256sum --check SHA256SUMS
gh attestation verify ./omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst \
  --repo brdweb/omacalendar
sudo pacman -U ./omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst
```

Verify socket activation, launch, reported version, desktop entry, provider
reconnect, upgrade continuity, and `pacman -Rns omacalendar` on a clean current-
Omarchy profile. GitHub release packages do not provide automatic pacman/yay
updates; users must install a newer release package explicitly.

## 7. Keep source and binary AUR recipes ready

AUR publication follows the GitHub beta so every recipe points to immutable,
public assets. Use separate AUR package bases:

- `omacalendar`: builds the source archive generated from the signed release
  tag;
- `omacalendar-bin`: installs the published x86-64 binary archive.

Render the recipes using the release version and verified asset hashes. For
`1.0.0-beta.1`, the renderer emits Arch `pkgver=1.0.0beta1` while retaining
`_upstream_version=1.0.0-beta.1`; this keeps pacman's version ordering correct.

For each recipe:

```bash
makepkg --verifysource
makepkg --syncdeps --cleanbuild --clean --force
namcap PKGBUILD ./*.pkg.tar.zst
makepkg --printsrcinfo > .SRCINFO
git diff --check
```

Repeat the build in a current clean Arch chroot before publishing. Review the
package contents and `.SRCINFO`, commit only packaging sources, then push to the
matching `ssh://aur@aur.archlinux.org/<pkgbase>.git` repository. Never commit a
built package, downloaded archive, credential, or private key.

When AUR account creation is available and both listings are public, install the
binary recipe through Omarchy:

```bash
omarchy pkg aur add omacalendar-bin
```

At beta preparation time, the AUR had suspended new account registration and the
maintainer did not have an existing account. Do not block or misrepresent the
beta on that external condition. Preserve the validated recipes and publish
them later, or coordinate with an existing trusted AUR maintainer. A custom
pacman repository is not required for beta.

## 8. Monitor and correct

Monitor GitHub issues, AUR comments/flags, release workflow results, Google OAuth
status, and provider regressions. Never retag or replace a published asset. Fix
a released mistake with a new semantic version such as `1.0.0-beta.2`, update
the AUR recipes to the new immutable assets, and preserve the old release.
