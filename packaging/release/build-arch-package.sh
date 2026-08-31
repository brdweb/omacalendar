#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 VERSION STAGE_ROOT OUTPUT_DIRECTORY" >&2
  exit 2
fi
if [[ ${EUID} -eq 0 ]]; then
  echo "native Arch packages must be assembled by an unprivileged user" >&2
  exit 2
fi

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=packaging/release/version-lib.sh
source "${repository_root}/packaging/release/version-lib.sh"

release_version=$1
stage_root=$2
output_directory=$3
source_date_epoch=${SOURCE_DATE_EPOCH:-}

if ! validate_release_version "${release_version}"; then
  echo "release version must be canonical semantic versioning without build metadata" >&2
  exit 2
fi
if [[ ! -d ${stage_root}/usr ]]; then
  echo "staged /usr tree is missing: ${stage_root}/usr" >&2
  exit 2
fi
if [[ ! ${source_date_epoch} =~ ^[1-9][0-9]*$ ]]; then
  echo "SOURCE_DATE_EPOCH must be a positive Unix timestamp" >&2
  exit 2
fi

stage_root=$(realpath "${stage_root}")
mkdir -p "${output_directory}"
output_directory=$(realpath "${output_directory}")
if [[ ! -w ${output_directory} ]]; then
  echo "output directory is not writable: ${output_directory}" >&2
  exit 2
fi

package_version=$(arch_pkgver "${release_version}")
package_filename="omacalendar-${package_version}-1-x86_64.pkg.tar.zst"
package_path="${output_directory}/${package_filename}"
if [[ -e ${package_path} || -L ${package_path} ]]; then
  echo "refusing to overwrite existing native package: ${package_path}" >&2
  exit 1
fi

working_directory=/tmp/omacalendar-native-package-build
if [[ -e ${working_directory} || -L ${working_directory} ]]; then
  echo "native package work directory already exists: ${working_directory}" >&2
  exit 1
fi
mkdir -m 0700 "${working_directory}"
cleanup() {
  rm -rf -- "${working_directory}"
}
trap cleanup EXIT

mkdir -p "${working_directory}/payload"
cp -a "${stage_root}/usr" "${working_directory}/payload/usr"
while IFS= read -r -d '' payload_path; do
  touch -h --date="@${source_date_epoch}" "${payload_path}"
done < <(find "${working_directory}/payload" -print0)

tar \
  --create \
  --file="${working_directory}/payload.tar" \
  --format=gnu \
  --group=0 \
  --mtime="@${source_date_epoch}" \
  --numeric-owner \
  --owner=0 \
  --sort=name \
  -C "${working_directory}/payload" usr
payload_sha256=$(sha256sum "${working_directory}/payload.tar" | cut -d' ' -f1)

sed \
  -e "s/@ARCH_PKGVER@/${package_version}/g" \
  -e "s/@PAYLOAD_SHA256@/${payload_sha256}/g" \
  "${repository_root}/packaging/arch/PKGBUILD-release.in" \
  >"${working_directory}/PKGBUILD"

(
  cd "${working_directory}"
  export LC_ALL=C
  export PACKAGER='OmaCalendar release automation <noreply@omacalendar.invalid>'
  export PKGDEST="${output_directory}"
  export SOURCE_DATE_EPOCH="${source_date_epoch}"
  export TZ=UTC
  umask 022
  makepkg --clean --cleanbuild --nodeps --noconfirm
)

if [[ ! -f ${package_path} ]]; then
  echo "makepkg did not create the expected package: ${package_filename}" >&2
  exit 1
fi

"${repository_root}/packaging/release/verify-arch-package.sh" \
  "${release_version}" "${stage_root}" "${package_path}" \
  "${source_date_epoch}"
printf '%s\n' "${package_path}"
