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
if [[ $(uname -m) != x86_64 ]] || ! grep -Eq '^ID=("arch"|arch)$' /etc/os-release; then
  echo "the clean native package test requires an Arch Linux x86_64 container" >&2
  exit 2
fi
repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "${repository_root}/packaging/release/version-lib.sh"
release_version=$1
package_version=$(arch_pkgver "${release_version}")
package_path=$(realpath "$2")
if [[ $(pacman -Qp "${package_path}") != "omacalendar ${package_version}-1" ]]; then
  echo "native package identity does not match the requested version" >&2
  exit 1
fi
if pacman -Q omacalendar >/dev/null 2>&1; then
  echo "refusing to replace an existing OmaCalendar installation; use a fresh container" >&2
  exit 2
fi

# The minimal official image excludes /usr/share/doc. Use a temporary config
# that restores only this application's manuals, without changing host/image
# configuration or allowing unrelated documentation to mask dependencies.
pacman_config=$(mktemp /tmp/omacalendar-pacman-config.XXXXXX)
trap 'rm -f -- "${pacman_config}"' EXIT
awk '
  /^\[/ {
    if (in_options) print "NoExtract = !usr/share/doc/OmaCalendar*"
    in_options = $0 == "[options]"
  }
  { print }
  END { if (in_options) print "NoExtract = !usr/share/doc/OmaCalendar*" }
' /etc/pacman.conf >"${pacman_config}"

# Install only the package and its declared dependencies after updating the
# rolling base image. Extra build or diagnostic packages could hide omissions
# in the native package's runtime dependency list.
pacman --config "${pacman_config}" -Syu --noconfirm
pacman --config "${pacman_config}" -U --noconfirm "${package_path}"
[[ $(pacman -Q omacalendar) == "omacalendar ${package_version}-1" ]]
pacman --config "${pacman_config}" -Qkk omacalendar
for command in secret-tool python3 git dbus-run-session; do
  command -v "${command}" >/dev/null
done

required_paths=(
  /usr/bin/omacalendar
  /usr/bin/omacalendar-widgetctl
  /usr/bin/omacalendarctl
  /usr/bin/omacalendard
  /usr/lib/systemd/user/omacalendard.service
  /usr/lib/systemd/user/omacalendard.socket
  /usr/share/applications/org.omacalendar.OmaCalendar.desktop
  /usr/share/icons/hicolor/scalable/apps/org.omacalendar.OmaCalendar.svg
  /usr/share/metainfo/org.omacalendar.OmaCalendar.metainfo.xml
  /usr/share/licenses/omacalendar/LICENSE
  /usr/share/doc/OmaCalendar/docs/GETTING_STARTED.md
  /usr/share/doc/OmaCalendar/docs/INSTALL.md
  /usr/share/doc/OmaCalendar/docs/OWNER_TESTING.md
  /usr/share/doc/OmaCalendar/docs/BACKUP_AND_RECOVERY.md
  /usr/share/doc/OmaCalendar/docs/UNINSTALL.md
  /usr/share/doc/OmaCalendar/docs/STABLE_PLAN.md
)
for installed_path in "${required_paths[@]}"; do
  [[ -f ${installed_path} ]]
  [[ $(pacman -Qoq -- "${installed_path}") == omacalendar ]]
done
for binary in omacalendar omacalendar-widgetctl omacalendarctl omacalendard; do
  [[ -x /usr/bin/${binary} ]]
done
grep -Fxq 'ExecStart=/usr/bin/omacalendard' \
  /usr/lib/systemd/user/omacalendard.service
grep -Fxq 'ListenStream=%t/omacalendar/daemon.sock' \
  /usr/lib/systemd/user/omacalendard.socket

unset LD_LIBRARY_PATH PKG_CONFIG_PATH
for binary in /usr/bin/omacalendar /usr/bin/omacalendard /usr/bin/omacalendarctl; do
  dependencies=$(ldd "${binary}")
  if grep -Fq 'not found' <<<"${dependencies}"; then
    echo "installed package has an unresolved runtime library: ${binary}" >&2
    printf '%s\n' "${dependencies}" >&2
    exit 1
  fi
done

# Each smoke harness creates private temporary XDG directories and cleans up
# its processes. No default native profile or live user systemd is accessed.
dbus-run-session -- bash "${repository_root}/packaging/release/smoke-daemon.sh" \
  /usr/bin "${release_version}"
for scale in 1 1.25 2; do
  QT_SCALE_FACTOR="${scale}" dbus-run-session -- \
    bash "${repository_root}/packaging/release/smoke-app.sh" /usr/bin
done

mapfile -t package_paths < <(pacman -Qlq omacalendar)
owned_files=()
for installed_path in "${package_paths[@]}"; do
  # Shared directory ancestors remain installed after removing this package.
  if [[ ${installed_path} != */ ]]; then
    owned_files+=("${installed_path}")
  fi
done
[[ ${#owned_files[@]} -gt 0 ]]
pacman -R --noconfirm omacalendar
if pacman -Q omacalendar >/dev/null 2>&1; then
  echo "package removal left an installed package registration" >&2
  exit 1
fi
for removed in "${owned_files[@]}"; do
  if [[ -e ${removed} || -L ${removed} ]]; then
    echo "package removal left an owned payload file: ${removed}" >&2
    exit 1
  fi
done
echo "Arch clean runtime install, owned documentation, library resolution, daemon restart, desktop scales and removal passed"
