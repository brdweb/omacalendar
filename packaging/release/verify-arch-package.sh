#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 VERSION STAGE_ROOT PACKAGE SOURCE_DATE_EPOCH" >&2
  exit 2
fi

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=packaging/release/version-lib.sh
source "${repository_root}/packaging/release/version-lib.sh"

release_version=$1
stage_root=$2
package_path=$3
source_date_epoch=$4
if ! validate_release_version "${release_version}"; then
  echo "release version must be canonical semantic versioning without build metadata" >&2
  exit 2
fi
if [[ ! -d ${stage_root}/usr ]]; then
  echo "staged /usr tree is missing: ${stage_root}/usr" >&2
  exit 2
fi
if [[ ! -f ${package_path} ]]; then
  echo "native Arch package is missing: ${package_path}" >&2
  exit 2
fi
if [[ ! ${source_date_epoch} =~ ^[1-9][0-9]*$ ]]; then
  echo "SOURCE_DATE_EPOCH must be a positive Unix timestamp" >&2
  exit 2
fi

stage_root=$(realpath "${stage_root}")
package_path=$(realpath "${package_path}")
package_version=$(arch_pkgver "${release_version}")
expected_filename="omacalendar-${package_version}-1-x86_64.pkg.tar.zst"
if [[ $(basename "${package_path}") != "${expected_filename}" ]]; then
  echo "native package filename must be ${expected_filename}" >&2
  exit 1
fi

package_metadata=$(bsdtar -xOf "${package_path}" .PKGINFO)
for expected_field in \
  'pkgname = omacalendar' \
  "pkgver = ${package_version}-1" \
  "builddate = ${source_date_epoch}" \
  'arch = x86_64'; do
  if ! grep -Fxq "${expected_field}" <<<"${package_metadata}"; then
    echo "native package metadata is missing: ${expected_field}" >&2
    exit 1
  fi
done

build_metadata=$(bsdtar -xOf "${package_path}" .BUILDINFO)
for expected_field in \
  "builddate = ${source_date_epoch}" \
  'builddir = /tmp/omacalendar-native-package-build' \
  'startdir = /tmp/omacalendar-native-package-build'; do
  if ! grep -Fxq "${expected_field}" <<<"${build_metadata}"; then
    echo "native package build metadata is missing: ${expected_field}" >&2
    exit 1
  fi
done

pacman -Qip "${package_path}" >/dev/null
pacman -Qlp "${package_path}" >/dev/null
unexpected_paths=$(bsdtar -tf "${package_path}" | \
  grep -Ev '^(\.BUILDINFO|\.MTREE|\.PKGINFO|usr/)' || true)
if [[ -n ${unexpected_paths} ]]; then
  echo "native package contains paths outside its metadata and /usr payload" >&2
  printf '%s\n' "${unexpected_paths}" >&2
  exit 1
fi
if ! bsdtar -tvf "${package_path}" | awk '
    { if ($3 != "root" || $4 != "root") exit 1; entries += 1 }
    END { if (entries == 0) exit 1 }
  '; then
  echo "native package entries must all be owned by root:root" >&2
  exit 1
fi

temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/omacalendar-package-verify.XXXXXX")
cleanup() {
  rm -rf -- "${temporary_root}"
}
trap cleanup EXIT
mkdir -p "${temporary_root}/extracted"
bsdtar -xf "${package_path}" -C "${temporary_root}/extracted"

packaged_license="${temporary_root}/extracted/usr/share/licenses/omacalendar/LICENSE"
staged_license="${stage_root}/usr/share/licenses/omacalendar/LICENSE"
documentation_license="${stage_root}/usr/share/doc/OmaCalendar/LICENSE"
if [[ ! -f ${packaged_license} ]] || \
  ! cmp -s "${documentation_license}" "${packaged_license}"; then
  echo "native package is missing its canonical MIT license copy" >&2
  exit 1
fi
if [[ ! -e ${staged_license} ]]; then
  rm -- "${packaged_license}"
  rmdir --ignore-fail-on-non-empty \
    "${temporary_root}/extracted/usr/share/licenses/omacalendar" \
    "${temporary_root}/extracted/usr/share/licenses"
fi

(
  cd "${stage_root}"
  find usr -printf '%P|%y|%m|%l\n' | LC_ALL=C sort
) >"${temporary_root}/expected-manifest"
(
  cd "${temporary_root}/extracted"
  find usr -printf '%P|%y|%m|%l\n' | LC_ALL=C sort
) >"${temporary_root}/actual-manifest"

if ! diff -u "${temporary_root}/expected-manifest" \
  "${temporary_root}/actual-manifest"; then
  echo "native package paths, types, modes, or symlinks differ from the staged tree" >&2
  exit 1
fi
if ! diff -qr --no-dereference "${stage_root}/usr" \
  "${temporary_root}/extracted/usr"; then
  echo "native package contents differ from the staged tree" >&2
  exit 1
fi

echo "native Arch package metadata and staged contents are valid"
