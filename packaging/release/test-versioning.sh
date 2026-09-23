#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=packaging/release/version-lib.sh
source "${repository_root}/packaging/release/version-lib.sh"

for version in 0.0.0 1.0.0 1.0.0-alpha 1.0.0-alpha.1 2.3.4-rc-1; do
  if ! validate_release_version "${version}"; then
    echo "valid release version was rejected: ${version}" >&2
    exit 1
  fi
done

for version in v1.0.0 01.0.0 1.00.0 1.0.00 1.0 1.0.0- 1.0.0-alpha..1 \
  1.0.0-alpha.01 1.0.0+build; do
  if validate_release_version "${version}"; then
    echo "invalid release version was accepted: ${version}" >&2
    exit 1
  fi
done

while read -r version expected_support_line; do
  if ! actual_support_line=$(release_support_line "${version}"); then
    echo "valid release version was rejected for support-line derivation: ${version}" >&2
    exit 1
  fi
  if [[ ${actual_support_line} != "${expected_support_line}" ]]; then
    echo "${version} became ${actual_support_line}, expected ${expected_support_line}" >&2
    exit 1
  fi
done <<'SUPPORT_LINES'
1.1.0 1.1
1.1.0-rc.1 1.1
1.1.0-beta.2 1.1
10.12.3-rc.4 10.12
SUPPORT_LINES

for version in v1.1.0 1.1 1.1.0- 1.1.0-beta.01 1.1.0+build; do
  if release_support_line "${version}" >/dev/null; then
    echo "invalid release version was accepted for support-line derivation: ${version}" >&2
    exit 1
  fi
done

if requires_public_release_gates 1.0.0-alpha; then
  echo "the already-published 1.0.0-alpha unexpectedly requires the expanded release gates" >&2
  exit 1
fi
for version in 1.0.0-alpha.2 1.0.0-beta.1 1.0.0-rc.1 1.0.0-rc.2 1.0.0-rc.3 1.0.0; do
  if ! requires_public_release_gates "${version}"; then
    echo "future public release did not require the expanded gates: ${version}" >&2
    exit 1
  fi
done

configured_version=$(cmake_release_version "${repository_root}")
# The active install guide must contain the current native package version and
# must not advertise retired binary formats.
expected_arch="omacalendar-$(arch_pkgver "${configured_version}")-1-x86_64.pkg.tar.zst"
grep -Fq "[Native Arch release ${configured_version}](https://github.com/brdweb/omacalendar/releases/tag/v${configured_version})" \
  "${repository_root}/README.md"
grep -Fq "OmaCalendar ${configured_version} supports" "${repository_root}/docs/INSTALL.md"
grep -Fxq "version=${configured_version}" "${repository_root}/docs/INSTALL.md"
grep -Fq "${expected_arch}" "${repository_root}/docs/INSTALL.md"
if grep -Eqi 'omacalendar_.*amd64\.deb|omacalendar-.*\.flatpak|^## (Ubuntu|Flatpak)' \
  "${repository_root}/docs/INSTALL.md"; then
  echo "active installation guide advertises a retired package format" >&2
  exit 1
fi
release_workflow="${repository_root}/.github/workflows/release.yml"
grep -Fq 'arch-candidate' "${release_workflow}"
grep -Eq "^[[:space:]]+default: ${configured_version}$" "${release_workflow}"
if grep -Eqi 'debian-candidate|flatpak-candidate|artifacts/.*linux-x86_64\.tar|PKGBUILD-bin' \
  "${release_workflow}"; then
  echo "release workflow still produces a retired binary distribution" >&2
  exit 1
fi
for retired_path in packaging/aur packaging/debian; do
  if [[ -e ${repository_root}/${retired_path} ]]; then
    echo "retired packaging path still exists: ${retired_path}" >&2
    exit 1
  fi
done
configured_minor=$(release_support_line "${configured_version}")
grep -Fq "OmaCalendar ${configured_minor} supports" "${repository_root}/SUPPORT.md"
grep -Fq "OmaCalendar ${configured_minor} is" "${repository_root}/docs/PLAN.md"
grep -Fq "latest ${configured_minor}.x release" "${repository_root}/SECURITY.md"
"${repository_root}/packaging/release/verify-release-metadata.sh" \
  "${configured_version}"
acceptance_record="${repository_root}/docs/releases/${configured_version}.md"
for invalid_draft in 1.0.0 1.0.0-beta.1 1.0.0-rc 1.0.0-rc.01; do
  if bash "${repository_root}/packaging/release/verify-draft-candidate.sh" \
    "${invalid_draft}" >/dev/null 2>&1; then
    echo "draft-only validation accepted an ineligible version: ${invalid_draft}" >&2
    exit 1
  fi
done
if [[ ${configured_version} == *-rc.* ]]; then
  bash "${repository_root}/packaging/release/verify-draft-candidate.sh" \
    "${configured_version}"
fi
if awk '
    BEGIN { FS = "\\|" }
    $0 == "## Pre-tag gates" || $0 == "## External approvals" {
      in_required_section = 1
      next
    }
    in_required_section && /^## / { in_required_section = 0 }
    in_required_section && /^\|/ {
      status = $3
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", status)
      if (status ~ /^PENDING([[:space:]]+-.*)?$/) {
        pending = 1
      }
    }
    END { exit pending ? 0 : 1 }
  ' "${acceptance_record}"; then
  if "${repository_root}/packaging/release/verify-release-metadata.sh" \
    --require-pretag-pass "${configured_version}" >/dev/null 2>&1; then
    echo "pending pre-tag gates unexpectedly passed strict release verification" >&2
    exit 1
  fi
else
  "${repository_root}/packaging/release/verify-release-metadata.sh" \
    --require-pretag-pass "${configured_version}"
fi

while read -r version expected_package_version; do
  if ! actual_package_version=$(arch_pkgver "${version}"); then
    echo "supported Arch release version was rejected: ${version}" >&2
    exit 1
  fi
  if [[ ${actual_package_version} != "${expected_package_version}" ]]; then
    echo "${version} became ${actual_package_version}, expected ${expected_package_version}" >&2
    exit 1
  fi
done <<'SUPPORTED_ARCH_VERSIONS'
1.0.0 1.0.0
1.0.0-alpha 1.0.0alpha
1.0.0-alpha.0 1.0.0alpha0
1.0.0-beta 1.0.0beta
1.0.0-beta.1 1.0.0beta1
1.0.0-rc 1.0.0rc
1.0.0-rc.1 1.0.0rc1
1.0.0-rc.2 1.0.0rc2
1.0.0-rc.3 1.0.0rc3
1.0.0-rc.12 1.0.0rc12
SUPPORTED_ARCH_VERSIONS

for version in \
  1.0.0-rc-1 \
  1.0.0-beta1 \
  1.0.0-preview \
  1.0.0-alpha-1 \
  1.0.0-beta.1.2 \
  1.0.0-1; do
  if arch_pkgver "${version}" >/dev/null; then
    echo "unsupported or colliding Arch release version was accepted: ${version}" >&2
    exit 1
  fi
done

if [[ $(vercmp 1.0.0beta1 1.0.0) -ge 0 ]] || \
  [[ $(vercmp 1.0.0beta1 1.0.0beta2) -ge 0 ]] || \
  [[ $(vercmp 1.0.0rc1 1.0.0rc2) -ge 0 ]] || \
  [[ $(vercmp 1.0.0rc2 1.0.0rc3) -ge 0 ]] || \
  [[ $(vercmp 1.0.0rc3 1.0.0) -ge 0 ]]; then
  echo "Arch prerelease pkgver ordering is invalid" >&2
  exit 1
fi

# A waived gate stands in for a pass only when it states a reason. Exercise both
# outcomes against a scratch copy so the live acceptance record stays untouched.
scratch_root=$(mktemp -d)
trap 'rm -rf "${scratch_root}"' EXIT
while IFS= read -r tracked_path; do
  install -D "${repository_root}/${tracked_path}" "${scratch_root}/${tracked_path}"
done <<SCRATCH_PATHS
CMakeLists.txt
CHANGELOG.md
SECURITY.md
SUPPORT.md
docs/BACKUP_AND_RECOVERY.md
docs/COMPATIBILITY.md
docs/PRIVACY.md
docs/RELEASE.md
docs/UNINSTALL.md
docs/releases/${configured_version}.md
packaging/org.omacalendar.OmaCalendar.metainfo.xml
packaging/release/verify-release-metadata.sh
packaging/release/version-lib.sh
src/core/domain.h
SCRATCH_PATHS

scratch_record="${scratch_root}/docs/releases/${configured_version}.md"
scratch_verify="${scratch_root}/packaging/release/verify-release-metadata.sh"
first_pretag_gate="Exact app version metadata and prerelease tooling"

rewrite_first_pretag_status() {
  local replacement=$1
  awk -v gate="${first_pretag_gate}" -v replacement="${replacement}" '
    $0 == "## Pre-tag gates" { in_section = 1 }
    in_section && /^## / && $0 != "## Pre-tag gates" { in_section = 0 }
    in_section && index($0, "| " gate " | ") == 1 && !done {
      split($0, cells, "|")
      printf "|%s| %s |%s|\n", cells[2], replacement, cells[4]
      done = 1
      next
    }
    { print }
  ' "${scratch_record}" >"${scratch_record}.new"
  mv "${scratch_record}.new" "${scratch_record}"
}

rewrite_first_pretag_status "WAIVED - owner fast-track"
if ! "${scratch_verify}" --require-pretag-pass "${configured_version}" >/dev/null; then
  echo "a waived pre-tag gate with a reason was rejected by strict verification" >&2
  exit 1
fi

rewrite_first_pretag_status "WAIVED"
if "${scratch_verify}" "${configured_version}" >/dev/null 2>&1; then
  echo "a waived pre-tag gate without a reason was accepted" >&2
  exit 1
fi

echo "release versioning contracts passed"
