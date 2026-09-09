#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 VERSION STAGE_ROOT OUTPUT_DIRECTORY" >&2
  exit 2
fi
repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "${repository_root}/packaging/release/version-lib.sh"
source "${repository_root}/packaging/release/deb-version-lib.sh"
release_version=$1
stage_root=$(realpath "$2")
package_version=$(deb_package_version "${release_version}")
package_filename=$(deb_package_filename "${release_version}")
if [[ ! ${SOURCE_DATE_EPOCH:-} =~ ^[1-9][0-9]*$ ]]; then
  echo "SOURCE_DATE_EPOCH must be a positive Unix timestamp" >&2
  exit 2
fi
if [[ $(dpkg --print-architecture) != amd64 ]]; then
  echo "Debian release packages currently require an amd64 build environment" >&2
  exit 2
fi
for required in usr/bin/omacalendar usr/bin/omacalendard usr/bin/omacalendarctl \
  usr/lib/omacalendar/libical.so.4.0 usr/share/doc/omacalendar/copyright; do
  if [[ ! -f ${stage_root}/${required} ]]; then
    echo "required Debian payload is missing: ${required}" >&2
    exit 2
  fi
done
mkdir -p "$3"
output_directory=$(realpath "$3")
package_path="${output_directory}/${package_filename}"
if [[ -e ${package_path} || -L ${package_path} ]]; then
  echo "refusing to overwrite existing Debian package: ${package_path}" >&2
  exit 1
fi

working_directory=$(mktemp -d /tmp/omacalendar-deb-package.XXXXXX)
trap 'rm -rf -- "${working_directory}"' EXIT
mkdir -p "${working_directory}/payload/DEBIAN" "${working_directory}/debian"
cp -a "${stage_root}/usr" "${working_directory}/payload/"
printf 'Source: omacalendar\n\nPackage: omacalendar\nArchitecture: amd64\n' \
  >"${working_directory}/debian/control"
# The private SONAME belongs to this package and must not create a dependency
# on a nonexistent Debian libical4 package. All other ELF dependencies are
# resolved against Debian's installed shlibs/symbols metadata.
printf 'libical 4.0 omacalendar (= %s)\n' "${package_version}" \
  >"${working_directory}/debian/shlibs.local"
shlib_dependencies=$(
  cd "${working_directory}"
  dpkg-shlibdeps -O -xomacalendar \
    -l"${working_directory}/payload/usr/lib/omacalendar" \
    -e"${working_directory}/payload/usr/bin/omacalendar" \
    -e"${working_directory}/payload/usr/bin/omacalendard" \
    -e"${working_directory}/payload/usr/bin/omacalendarctl" \
    -e"${working_directory}/payload/usr/lib/omacalendar/libical.so.4.0" |
    sed -n 's/^shlibs:Depends=//p'
)
if [[ -z ${shlib_dependencies} ]]; then
  echo "dpkg-shlibdeps did not resolve runtime dependencies" >&2
  exit 1
fi
installed_size=$(du -sk "${working_directory}/payload/usr" | cut -f1)
sed -e "s/@DEB_VERSION@/${package_version}/g" \
  -e "s/@INSTALLED_SIZE@/${installed_size}/g" \
  -e "s/@SHLIB_DEPENDS@/${shlib_dependencies}/g" \
  "${repository_root}/packaging/debian/control.in" \
  >"${working_directory}/payload/DEBIAN/control"
(
  cd "${working_directory}/payload"
  find usr -type f -print0 | LC_ALL=C sort -z | xargs -0 md5sum \
    >DEBIAN/md5sums
  find . -print0 | xargs -0 touch -h --date="@${SOURCE_DATE_EPOCH}"
)
chmod 0755 "${working_directory}/payload/DEBIAN"
chmod 0644 "${working_directory}/payload/DEBIAN/"*
LC_ALL=C TZ=UTC dpkg-deb --root-owner-group --uniform-compression \
  -Zxz -z9 --threads-max=1 --build "${working_directory}/payload" "${package_path}"
bash "${repository_root}/packaging/release/verify-deb-package.sh" \
  "${release_version}" "${stage_root}" "${package_path}"
printf '%s\n' "${package_path}"
