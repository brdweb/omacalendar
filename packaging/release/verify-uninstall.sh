#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 STAGED_ROOT CMAKE_INSTALL_MANIFEST" >&2
  exit 2
fi

staged_root=$(realpath "$1")
install_manifest=$(realpath "$2")
if [[ ! -d ${staged_root} || ${staged_root} == / || ${staged_root} == "${HOME}" ||
      ${staged_root} == "$(pwd)" ]]; then
  echo "refusing unsafe staged root: ${staged_root}" >&2
  exit 2
fi

while IFS= read -r installed_path || [[ -n ${installed_path} ]]; do
  [[ -z ${installed_path} ]] && continue
  if [[ ${installed_path} != /usr/* || ${installed_path} == *'..'* ]]; then
    echo "refusing unexpected manifest path: ${installed_path}" >&2
    exit 2
  fi
  rm -f -- "${staged_root}/${installed_path#/}"
done <"${install_manifest}"

package_paths=(
  usr/bin/omacalendar
  usr/bin/omacalendarctl
  usr/bin/omacalendard
  usr/lib/systemd/user/omacalendard.service
  usr/lib/systemd/user/omacalendard.socket
  usr/share/applications/org.omacalendar.OmaCalendar.desktop
  usr/share/icons/hicolor/scalable/apps/org.omacalendar.OmaCalendar.svg
  usr/share/metainfo/org.omacalendar.OmaCalendar.metainfo.xml
)

for relative_path in "${package_paths[@]}"; do
  if [[ -e "${staged_root}/${relative_path}" ]]; then
    echo "package-owned path remains after simulated uninstall: ${relative_path}" >&2
    exit 1
  fi
done

echo "simulated package uninstall removed every package-owned artifact"
