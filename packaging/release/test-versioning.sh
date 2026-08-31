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

if requires_public_release_gates 1.0.0-alpha; then
  echo "the already-published 1.0.0-alpha unexpectedly requires the expanded release gates" >&2
  exit 1
fi
for version in 1.0.0-alpha.2 1.0.0-beta.1 1.0.0-rc.1 1.0.0; do
  if ! requires_public_release_gates "${version}"; then
    echo "future public release did not require the expanded gates: ${version}" >&2
    exit 1
  fi
done

configured_version=$(cmake_release_version "${repository_root}")
"${repository_root}/packaging/release/verify-release-metadata.sh" \
  "${configured_version}"
acceptance_record="${repository_root}/docs/releases/${configured_version}.md"
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
  [[ $(vercmp 1.0.0beta1 1.0.0beta2) -ge 0 ]]; then
  echo "Arch prerelease pkgver ordering is invalid" >&2
  exit 1
fi

echo "release versioning contracts passed"
