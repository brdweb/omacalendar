#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=version-lib.sh
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
  1.0.0 "${checksum}" "${checksum}" "${temporary_root}/aur"
if "${repository_root}/packaging/release/render-aur.sh" \
  1.0.0-alpha "${checksum}" "${checksum}" "${temporary_root}/invalid-aur" \
  >/dev/null 2>&1; then
  echo "prerelease version unexpectedly rendered an AUR package" >&2
  exit 1
fi

echo "release versioning contracts passed"
