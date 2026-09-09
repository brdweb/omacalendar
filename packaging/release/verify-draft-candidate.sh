#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# This is a draft-assembly check, not a public-release acceptance bypass.
# Stable and beta tags must continue through strict verify-release.sh.
if [[ $# -ne 1 || ! $1 =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-rc\.(0|[1-9][0-9]*)$ ]]; then
  echo "draft preparation requires a canonical MAJOR.MINOR.PATCH-rc.N version" >&2
  exit 2
fi
version=$1
"${repository_root}/packaging/release/verify-release-metadata.sh" "${version}"
record="${repository_root}/docs/releases/${version}.md"
if ! grep -Fxq 'Release disposition: DRAFT ONLY; owner acceptance pending.' "${record}"; then
  echo "candidate record must explicitly restrict preparation to a draft" >&2
  exit 1
fi
for approval in \
  'Historical Google installed-app OAuth credential revoked or rotated' \
  'Repository-history hygiene decision recorded' \
  'Maintainer approval to create the signed app tag'; do
  if ! awk -F '|' -v label="${approval}" '
      $0 == "## External approvals" { section = 1; next }
      section && /^## / { section = 0 }
      section && /^\|/ {
        name = $2; status = $3
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", name)
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", status)
        if (name == label) { count++; approved = status == "APPROVED" }
      }
      END { exit !(count == 1 && approved) }
    ' "${record}"; then
    echo "draft preparation lacks required approval: ${approval}" >&2
    exit 1
  fi
done
echo "draft-only ${version} metadata passed; stable/owner gates remain separate"
