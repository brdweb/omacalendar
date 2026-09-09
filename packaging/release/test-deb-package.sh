#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 || ${EUID} -ne 0 ]]; then
  echo "usage (as container root): $0 VERSION PACKAGE" >&2
  exit 2
fi
if [[ ! -e /.dockerenv && ! -e /run/.containerenv ]]; then
  echo "package installation/removal verification must run in a disposable container" >&2
  exit 2
fi
repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "${repository_root}/packaging/release/version-lib.sh"
source "${repository_root}/packaging/release/deb-version-lib.sh"
release_version=$1
package_version=$(deb_package_version "${release_version}")
package_path=$(realpath "$2")
[[ $(basename "${package_path}") == "$(deb_package_filename "${release_version}")" ]]
[[ $(dpkg-deb --field "${package_path}" Version) == "${package_version}" ]]
[[ $(dpkg-deb --field "${package_path}" Package) == omacalendar ]]
apt-get update
# The official minimal Ubuntu image excludes /usr/share/doc by default. Restore
# this package's documentation so the clean-install test verifies the manuals
# and bundled dependency license notices as well as the executable payload.
DEBIAN_FRONTEND=noninteractive apt-get \
  -o 'Dpkg::Options::=--path-include=/usr/share/doc/OmaCalendar*' \
  -o 'Dpkg::Options::=--path-include=/usr/share/doc/omacalendar*' \
  install -y --no-install-recommends "${package_path}"
[[ $(dpkg-query -W -f='${Status}' omacalendar) == 'install ok installed' ]]
[[ -z $(dpkg --verify omacalendar) ]]
command -v secret-tool >/dev/null
unset LD_LIBRARY_PATH PKG_CONFIG_PATH
for binary in /usr/bin/omacalendar /usr/bin/omacalendard /usr/bin/omacalendarctl; do
  if ldd "${binary}" | grep -F 'not found'; then
    echo "installed package has an unresolved runtime library: ${binary}" >&2
    exit 1
  fi
done
if ! ldd /usr/bin/omacalendard | grep -Fq '/usr/lib/omacalendar/libical.so.4.0'; then
  echo "installed daemon is not using the packaged private libical" >&2
  exit 1
fi
dbus-run-session -- bash "${repository_root}/packaging/release/smoke-daemon.sh" \
  /usr/bin "${release_version}"
for scale in 1 1.25 2; do
  QT_SCALE_FACTOR="${scale}" dbus-run-session -- \
    bash "${repository_root}/packaging/release/smoke-app.sh" /usr/bin
done
DEBIAN_FRONTEND=noninteractive apt-get purge -y omacalendar
for removed in /usr/bin/omacalendar /usr/bin/omacalendard /usr/bin/omacalendarctl \
  /usr/lib/systemd/user/omacalendard.service /usr/lib/systemd/user/omacalendard.socket \
  /usr/lib/omacalendar/libical.so.4.0; do
  if [[ -e ${removed} || -L ${removed} ]]; then
    echo "package removal left a payload file: ${removed}" >&2
    exit 1
  fi
done
echo "Debian clean runtime install, private library resolution, daemon restart, desktop scales and removal passed"
