#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 VERSION STAGE_ROOT PACKAGE" >&2
  exit 2
fi
repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "${repository_root}/packaging/release/version-lib.sh"
source "${repository_root}/packaging/release/deb-version-lib.sh"
package_version=$(deb_package_version "$1")
package_filename=$(deb_package_filename "$1")
stage_root=$(realpath "$2")
package_path=$(realpath "$3")
[[ $(basename "${package_path}") == "${package_filename}" ]]
[[ $(dpkg-deb --field "${package_path}" Package) == omacalendar ]]
[[ $(dpkg-deb --field "${package_path}" Version) == "${package_version}" ]]
[[ $(dpkg-deb --field "${package_path}" Architecture) == amd64 ]]
depends=$(dpkg-deb --field "${package_path}" Depends)
for required in libqt6core6t64 libqt6sql6-sqlite libsecret-tools qml6-module-qtquick-controls \
  qml6-module-qtquick-dialogs qt6-wayland ca-certificates tzdata; do
  if [[ ${depends} != *"${required}"* ]]; then
    echo "Debian runtime dependency missing: ${required}" >&2
    exit 1
  fi
done
if grep -Eq '(^|, )libical|(^|, )omacalendar([ ,(]|$)' <<<"${depends}"; then
  echo "Debian package must resolve libical through its private payload" >&2
  exit 1
fi
if ! dpkg-deb --contents "${package_path}" | awk '
  { if ($2 != "root/root") exit 1; entries += 1 }
  END { if (entries == 0) exit 1 }
'; then
  echo "Debian package entries must be owned by root:root" >&2
  exit 1
fi
if dpkg-deb --fsys-tarfile "${package_path}" | tar -tf - | \
  grep -Ev '^(\./|\./usr/.*)$'; then
  echo "Debian package includes paths outside /usr" >&2
  exit 1
fi
temporary_root=$(mktemp -d /tmp/omacalendar-deb-verify.XXXXXX)
trap 'rm -rf -- "${temporary_root}"' EXIT
dpkg-deb --raw-extract "${package_path}" "${temporary_root}"
(
  cd "${temporary_root}"
  md5sum --check --status DEBIAN/md5sums
)
diff -qr --no-dereference "${stage_root}/usr" "${temporary_root}/usr"
diff -u \
  <(cd "${stage_root}" && find usr -printf '%P|%y|%m|%l\n' | LC_ALL=C sort) \
  <(cd "${temporary_root}" && find usr -printf '%P|%y|%m|%l\n' | LC_ALL=C sort)
for binary in omacalendar omacalendard; do
  if ! readelf -d "${temporary_root}/usr/bin/${binary}" | \
    grep -Eq '\(RUNPATH\).*\[/usr/lib/omacalendar(:|\])'; then
    echo "Debian ${binary} does not resolve its private libical installation" >&2
    exit 1
  fi
done
echo "Debian package metadata, runtime dependencies, and staged payload verified"
