#!/usr/bin/env bash
set -euo pipefail

require_pretag_pass=0
if [[ ${1:-} == --require-pretag-pass ]]; then
  require_pretag_pass=1
  shift
fi
if [[ $# -gt 1 ]]; then
  echo "usage: $0 [--require-pretag-pass] [MAJOR.MINOR.PATCH[-PRERELEASE]]" >&2
  exit 2
fi

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=version-lib.sh
source "${repository_root}/packaging/release/version-lib.sh"

configured_version=$(cmake_release_version "${repository_root}")
release_version=${1:-${configured_version}}
if ! validate_release_version "${release_version}"; then
  echo "release version must be canonical semantic versioning without build metadata" >&2
  exit 2
fi
if [[ ${configured_version} != "${release_version}" ]]; then
  echo "release version ${release_version} does not match CMake version ${configured_version}" >&2
  exit 1
fi

escaped_version=${release_version//./\.}
if ! grep -Eq "^## \\[${escaped_version}\\] - [0-9]{4}-[0-9]{2}-[0-9]{2}$" \
  "${repository_root}/CHANGELOG.md"; then
  echo "CHANGELOG.md has no dated ${release_version} release section" >&2
  exit 1
fi

release_type=stable
if [[ ${release_version} == *-* ]]; then
  release_type=development
fi
if ! grep -Eq \
  "<release version=\"${escaped_version}\" date=\"[0-9]{4}-[0-9]{2}-[0-9]{2}\" type=\"${release_type}\">" \
  "${repository_root}/packaging/org.omacalendar.OmaCalendar.metainfo.xml"; then
  echo "AppStream metadata has no ${release_type} ${release_version} release entry" >&2
  exit 1
fi

extract_domain_constant() {
  local constant_name=$1
  local domain_header="${repository_root}/src/core/domain.h"
  local pattern="^[[:space:]]*inline[[:space:]]+constexpr[[:space:]]+int[[:space:]]+${constant_name}[[:space:]]*=[[:space:]]*([0-9]+)[[:space:]]*;[[:space:]]*$"
  local line value=
  local matches=0

  while IFS= read -r line; do
    if [[ ${line} =~ ${pattern} ]]; then
      value=${BASH_REMATCH[1]}
      ((matches += 1))
    fi
  done <"${domain_header}"
  if [[ ${matches} -ne 1 ]]; then
    echo "${domain_header} must define exactly one numeric ${constant_name}" >&2
    return 1
  fi
  printf '%s' "${value}"
}

ipc_protocol_major=$(extract_domain_constant kIpcProtocolMajor)
ipc_protocol_minor=$(extract_domain_constant kIpcProtocolMinor)
ipc_protocol_version="${ipc_protocol_major}.${ipc_protocol_minor}"
if ! grep -Fq \
  "| \`${release_version}\` | ${ipc_protocol_version} | 2 |" \
  "${repository_root}/docs/COMPATIBILITY.md"; then
  echo "compatibility matrix has no exact app ${release_version}, IPC ${ipc_protocol_version}, schema 2 row" >&2
  exit 1
fi

acceptance_record="${repository_root}/docs/releases/${release_version}.md"
if [[ ! -s ${acceptance_record} ]]; then
  echo "release acceptance record is missing: docs/releases/${release_version}.md" >&2
  exit 1
fi
if ! grep -Fq \
  "App ${release_version}; IPC ${ipc_protocol_version}; schema 2" \
  "${acceptance_record}"; then
  echo "release acceptance record has no exact app, IPC, and schema identity" >&2
  exit 1
fi
for required_heading in \
  "## Candidate identity" \
  "## Pre-tag gates" \
  "## Post-tag draft gates" \
  "## Known limitations" \
  "## External approvals"; do
  heading_count=$(grep -Fxc "${required_heading}" "${acceptance_record}" || true)
  if [[ ${heading_count} -ne 1 ]]; then
    echo "release acceptance record must contain exactly one ${required_heading} heading" >&2
    exit 1
  fi
done

trim_whitespace() {
  local value=$1
  value=${value#"${value%%[![:space:]]*}"}
  value=${value%"${value##*[![:space:]]}"}
  printf '%s' "${value}"
}

declare -a PRETAG_GATE_LABELS=(
  "Exact app version metadata and prerelease tooling"
  "Clean GCC and Clang builds with full automated matrix"
  "ASan/UBSan matrix, including IPC framing/reconnect regressions"
  "100,000-event release performance harness without daemon crash"
  "Staged \`/usr\` install, desktop launch, daemon restart, and uninstall"
  "Local CRUD/offline cache, Radicale round trip, and public ICS refresh"
  "Secret scan and dependency review"
  "No unresolved critical or high defect"
)
declare -a POSTTAG_GATE_LABELS=(
  "Signed app tag and exact target commit independently verified"
  "Checksums and package-specific provenance/SBOM attestations verified"
  "Native Arch package installation, restart, and removal"
  "Complete owner and live-provider acceptance"
  "Maintainer approval to publish"
)
declare -a EXTERNAL_APPROVAL_LABELS=(
  "Historical Google installed-app OAuth credential revoked or rotated"
  "Repository-history hygiene decision recorded"
  "App repository committed, clean, pushed, and green in clean-checkout CI"
  "Maintainer approval to create the signed app tag"
)

if requires_public_release_gates "${release_version}"; then
  PRETAG_GATE_LABELS+=(
    "Current-Omarchy owner acceptance on the exact runtime"
    "Prior-release upgrade, backup/restore, and uninstall"
    "Privacy-safe screenshots and public release documentation"
    "Arch package passes clean validation"
  )
  EXTERNAL_APPROVAL_LABELS+=(
    "Google OAuth branding and sensitive-scope verification approved"
    "Maintainer approval of public release acceptance and limitations"
  )
fi

require_section_rows() {
  local section=$1
  local row_label=$2
  local expected_status_description=$3
  local expected_status_pattern=$4
  shift 4
  local -a expected_labels=("$@")
  local found_rows=0
  local failed=0
  local label status evidence _leading _trailing
  local expected_label
  local -A required_labels=()
  local -A seen_labels=()

  for expected_label in "${expected_labels[@]}"; do
    required_labels["${expected_label}"]=1
  done

  while IFS='|' read -r _leading label status evidence _trailing; do
    label=$(trim_whitespace "${label}")
    status=$(trim_whitespace "${status}")
    evidence=$(trim_whitespace "${evidence}")
    if [[ -z ${label} || ${label} == "${row_label}" || ${label} == ---* ]]; then
      continue
    fi
    if [[ -z ${required_labels["${label}"]+present} ]]; then
      echo "${section} contains an unexpected row: ${label}" >&2
      failed=1
      continue
    fi
    if [[ -n ${seen_labels["${label}"]+present} ]]; then
      echo "${section} contains a duplicate row: ${label}" >&2
      failed=1
      continue
    fi
    seen_labels["${label}"]=1
    ((found_rows += 1))
    if [[ ! ${status} =~ ${expected_status_pattern} ]]; then
      echo "${section} has invalid status for ${label}: ${status:-missing}; expected ${expected_status_description}" >&2
      failed=1
    fi
    if [[ -z ${evidence} ]]; then
      echo "${section} has no evidence note for ${label}" >&2
      failed=1
    fi
  done < <(
    awk -v heading="## ${section}" '
      $0 == heading { in_section = 1; next }
      in_section && /^## / { exit }
      in_section && /^\|/ { print }
    ' "${acceptance_record}"
  )

  for expected_label in "${expected_labels[@]}"; do
    if [[ -z ${seen_labels["${expected_label}"]+present} ]]; then
      echo "${section} is missing a required row: ${expected_label}" >&2
      failed=1
    fi
  done
  if [[ ${found_rows} -ne ${#expected_labels[@]} ]]; then
    failed=1
  fi
  [[ ${failed} -eq 0 ]]
}

acceptance_structure_failed=0
require_section_rows \
  "Pre-tag gates" "Gate" "PASS or PENDING" \
  '^(PASS|PENDING([[:space:]]+-.*)?)$' \
  "${PRETAG_GATE_LABELS[@]}" || acceptance_structure_failed=1
require_section_rows \
  "Post-tag draft gates" "Gate" "PASS, APPROVED, or PENDING" \
  '^(PASS|APPROVED|PENDING([[:space:]]+-.*)?)$' \
  "${POSTTAG_GATE_LABELS[@]}" || acceptance_structure_failed=1
require_section_rows \
  "External approvals" "Approval" "APPROVED or PENDING" \
  '^(APPROVED|PENDING([[:space:]]+-.*)?)$' \
  "${EXTERNAL_APPROVAL_LABELS[@]}" || acceptance_structure_failed=1
if [[ ${acceptance_structure_failed} -ne 0 ]]; then
  echo "release acceptance record has an invalid gate structure" >&2
  exit 1
fi

if [[ ${require_pretag_pass} -eq 1 ]]; then
  pretag_failed=0
  require_section_rows \
    "Pre-tag gates" "Gate" "PASS" '^PASS$' \
    "${PRETAG_GATE_LABELS[@]}" || pretag_failed=1
  require_section_rows \
    "External approvals" "Approval" "APPROVED" '^APPROVED$' \
    "${EXTERNAL_APPROVAL_LABELS[@]}" || pretag_failed=1
  if [[ ${pretag_failed} -ne 0 ]]; then
    echo "release acceptance record is not approved for tagging" >&2
    exit 1
  fi
fi

for required_file in \
  SECURITY.md SUPPORT.md \
  docs/BACKUP_AND_RECOVERY.md docs/COMPATIBILITY.md docs/PRIVACY.md \
  docs/RELEASE.md docs/UNINSTALL.md; do
  if [[ ! -s ${repository_root}/${required_file} ]]; then
    echo "required release documentation is missing: ${required_file}" >&2
    exit 1
  fi
done

if grep -rInE \
  '(client_secret|refresh_token|access_token)[[:space:]]*[:=][[:space:]]*[^[:space:]$<{]+' \
  --exclude-dir=.git --exclude-dir='build*' \
  "${repository_root}"; then
  echo "possible credential material found in the release tree" >&2
  exit 1
fi

echo "release metadata for ${release_version} is internally consistent"
