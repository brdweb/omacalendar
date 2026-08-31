#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 vMAJOR.MINOR.PATCH[-PRERELEASE]" >&2
  exit 2
fi

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=version-lib.sh
source "${repository_root}/packaging/release/version-lib.sh"

release_tag=$1
release_version=${release_tag#v}
if [[ ${release_tag} == "${release_version}" ]] || \
  ! validate_release_version "${release_version}"; then
  echo "release tag must be vMAJOR.MINOR.PATCH[-PRERELEASE]" >&2
  exit 2
fi

if [[ $(git -C "${repository_root}" cat-file -t "${release_tag}" 2>/dev/null || true) != tag ]]; then
  echo "${release_tag} must be an annotated tag" >&2
  exit 1
fi
if ! git -C "${repository_root}" cat-file tag "${release_tag}" | \
  grep -Eq '^-----BEGIN (PGP|SSH) SIGNATURE-----$'; then
  echo "${release_tag} must carry a PGP or SSH signature" >&2
  exit 1
fi

tag_commit=$(git -C "${repository_root}" rev-parse "${release_tag}^{commit}")
head_commit=$(git -C "${repository_root}" rev-parse HEAD)
if [[ ${tag_commit} != "${head_commit}" ]]; then
  echo "${release_tag} does not point at the checked-out commit" >&2
  exit 1
fi
if [[ -n $(git -C "${repository_root}" status --porcelain --untracked-files=all) ]]; then
  echo "release verification requires a clean checkout" >&2
  exit 1
fi

if [[ ${OMACALENDAR_GITHUB_VERIFIED_TAG:-0} != 1 ]]; then
  if ! git -C "${repository_root}" verify-tag "${release_tag}" >/dev/null; then
    echo "${release_tag} signature could not be verified by local Git trust" >&2
    exit 1
  fi
fi

"${repository_root}/packaging/release/verify-release-metadata.sh" \
  --require-pretag-pass "${release_version}"
echo "signed release candidate ${release_tag} is internally consistent"
