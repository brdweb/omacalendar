#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=packaging/release/version-lib.sh
source "${repository_root}/packaging/release/version-lib.sh"
source "${repository_root}/packaging/release/deb-version-lib.sh"

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
# Active install guides must not silently retain a superseded candidate.
for guide in docs/INSTALL.md packaging/flatpak/README.md; do
  mapfile -t examples < <(grep -Eo 'omacalendar-[0-9][0-9A-Za-z.-]*-linux-x86_64\.flatpak' \
    "${repository_root}/${guide}" | LC_ALL=C sort -u)
  if [[ ${#examples[@]} != 1 || ${examples[0]} != "omacalendar-${configured_version}-linux-x86_64.flatpak" ]]; then
    echo "${guide} has missing or stale Flatpak install examples" >&2
    exit 1
  fi
done
for guide in docs/INSTALL.md packaging/debian/README.md; do
  mapfile -t examples < <(grep -Eo 'omacalendar_[0-9][0-9A-Za-z.~+-]*_amd64\.deb' \
    "${repository_root}/${guide}" | LC_ALL=C sort -u)
  # The Debian packaging guide also explains the invariant stable filename.
  for example in "${examples[@]}"; do
    if [[ ${example} != "$(deb_package_filename "${configured_version}")" &&
          ${example} != "$(deb_package_filename "$(release_base_version "${configured_version}")")" ]]; then
      echo "${guide} has a stale Debian asset filename: ${example}" >&2
      exit 1
    fi
  done
  [[ ${#examples[@]} -gt 0 ]] || { echo "${guide} has no Debian filename example" >&2; exit 1; }
done
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

temporary_root=$(mktemp -d /tmp/omacalendar-release-versioning.XXXXXX)
cleanup() {
  rm -rf -- "${temporary_root}"
}
trap cleanup EXIT
checksum=$(printf '0%.0s' {1..64})
"${repository_root}/packaging/release/render-aur.sh" \
  1.0.0 "${checksum}" "${checksum}" "${temporary_root}/stable-aur"
"${repository_root}/packaging/release/render-aur.sh" \
  1.0.0-beta.1 "${checksum}" "${checksum}" "${temporary_root}/beta-aur"

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

# The control Version retains Debian's tilde ordering, but the download name
# must survive GitHub release-asset sanitization unchanged.
while read -r version expected_control expected_filename; do
  [[ $(deb_package_version "${version}") == "${expected_control}" ]]
  actual_filename=$(deb_package_filename "${version}")
  [[ ${actual_filename} == "${expected_filename}" ]]
  [[ ${actual_filename} =~ ^[A-Za-z0-9][A-Za-z0-9._-]*[A-Za-z0-9]$ ]]
  [[ ${actual_filename} != *'~'* ]]
done <<'SUPPORTED_DEBIAN_VERSIONS'
1.0.0 1.0.0-1 omacalendar_1.0.0-1_amd64.deb
1.0.0-alpha.0 1.0.0~alpha.0-1 omacalendar_1.0.0-alpha.0-1_amd64.deb
1.0.0-beta.1 1.0.0~beta.1-1 omacalendar_1.0.0-beta.1-1_amd64.deb
1.0.0-rc.1 1.0.0~rc.1-1 omacalendar_1.0.0-rc.1-1_amd64.deb
1.0.0-rc.2 1.0.0~rc.2-1 omacalendar_1.0.0-rc.2-1_amd64.deb
1.0.0-rc.3 1.0.0~rc.3-1 omacalendar_1.0.0-rc.3-1_amd64.deb
2.3.4-rc-1 2.3.4~rc-1-1 omacalendar_2.3.4-rc-1-1_amd64.deb
SUPPORTED_DEBIAN_VERSIONS
for invalid_version in '' v1.0.0 01.0.0 1.0.0-rc.03 1.0.0+build \
  '1.0.0~rc.3' '1.0.0-rc/3' '1.0.0-rc 3'; do
  if deb_package_filename "${invalid_version}"; then
    echo "invalid version produced a Debian asset filename: ${invalid_version}" >&2
    exit 1
  fi
done
if deb_package_filename || deb_package_filename 1.0.0 unexpected; then
  echo "Debian filename helper accepted the wrong argument count" >&2
  exit 1
fi

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

for package_kind in source binary; do
  stable_pkgbuild="${temporary_root}/stable-aur/${package_kind}/PKGBUILD"
  beta_pkgbuild="${temporary_root}/beta-aur/${package_kind}/PKGBUILD"
  bash -n "${stable_pkgbuild}" "${beta_pkgbuild}"
  grep -Fxq 'pkgver=1.0.0' "${stable_pkgbuild}"
  grep -Fxq 'pkgver=1.0.0beta1' "${beta_pkgbuild}"
  grep -Fxq '_upstream_version=1.0.0' "${stable_pkgbuild}"
  grep -Fxq '_upstream_version=1.0.0-beta.1' "${beta_pkgbuild}"
  if grep -Eq '@[A-Z_]+@' "${stable_pkgbuild}" "${beta_pkgbuild}"; then
    echo "rendered ${package_kind} PKGBUILD contains an unresolved template value" >&2
    exit 1
  fi
  (
    cd "$(dirname "${stable_pkgbuild}")"
    makepkg --printsrcinfo >.SRCINFO
  )
  (
    cd "$(dirname "${beta_pkgbuild}")"
    makepkg --printsrcinfo >.SRCINFO
  )
  grep -Fq $'\tpkgver = 1.0.0' "$(dirname "${stable_pkgbuild}")/.SRCINFO"
  grep -Fq $'\tpkgver = 1.0.0beta1' "$(dirname "${beta_pkgbuild}")/.SRCINFO"
done

grep -Fxq '    -DOMACALENDAR_VERSION_SUFFIX=' \
  "${temporary_root}/stable-aur/source/PKGBUILD"
grep -Fxq '    -DOMACALENDAR_VERSION_SUFFIX=-beta.1' \
  "${temporary_root}/beta-aur/source/PKGBUILD"
upstream_reference="\${_upstream_version}"
grep -Fq "releases/download/v${upstream_reference}/omacalendar-${upstream_reference}-source.tar.gz" \
  "${temporary_root}/beta-aur/source/PKGBUILD"
grep -Fq "releases/download/v${upstream_reference}/omacalendar-${upstream_reference}-linux-\${CARCH}.tar.zst" \
  "${temporary_root}/beta-aur/binary/PKGBUILD"

if [[ $(vercmp 1.0.0beta1 1.0.0) -ge 0 ]] || \
  [[ $(vercmp 1.0.0beta1 1.0.0beta2) -ge 0 ]] || \
  [[ $(vercmp 1.0.0rc1 1.0.0rc2) -ge 0 ]] || \
  [[ $(vercmp 1.0.0rc2 1.0.0rc3) -ge 0 ]] || \
  [[ $(vercmp 1.0.0rc3 1.0.0) -ge 0 ]]; then
  echo "Arch prerelease pkgver ordering is invalid" >&2
  exit 1
fi

echo "release versioning contracts passed"
