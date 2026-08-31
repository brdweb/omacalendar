#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 STAGED_ROOT" >&2
  exit 2
fi

staged_root=${1%/}
required_paths=(
  usr/bin/omacalendar
  usr/bin/omacalendarctl
  usr/bin/omacalendard
  usr/lib/systemd/user/omacalendard.service
  usr/lib/systemd/user/omacalendard.socket
  usr/share/applications/org.omacalendar.OmaCalendar.desktop
  usr/share/icons/hicolor/scalable/apps/org.omacalendar.OmaCalendar.svg
  usr/share/metainfo/org.omacalendar.OmaCalendar.metainfo.xml
)

for relative_path in "${required_paths[@]}"; do
  if [[ ! -e "${staged_root}/${relative_path}" ]]; then
    echo "missing installed path: ${relative_path}" >&2
    exit 1
  fi
done

for binary in omacalendar omacalendarctl omacalendard; do
  if [[ ! -x "${staged_root}/usr/bin/${binary}" ]]; then
    echo "installed binary is not executable: usr/bin/${binary}" >&2
    exit 1
  fi
done

expected_exec='ExecStart=/usr/bin/omacalendard'
if ! grep -Fxq "${expected_exec}" \
  "${staged_root}/usr/lib/systemd/user/omacalendard.service"; then
  echo "systemd unit is not configured for the /usr package prefix" >&2
  exit 1
fi
if ! grep -Fxq 'ListenStream=%t/omacalendar/daemon.sock' \
  "${staged_root}/usr/lib/systemd/user/omacalendard.socket"; then
  echo "systemd socket does not own the expected runtime endpoint" >&2
  exit 1
fi

desktop-file-validate \
  "${staged_root}/usr/share/applications/org.omacalendar.OmaCalendar.desktop"
appstreamcli validate --no-net \
  "${staged_root}/usr/share/metainfo/org.omacalendar.OmaCalendar.metainfo.xml"
if command -v systemd-analyze >/dev/null; then
  systemd-analyze security --offline=yes \
    "${staged_root}/usr/lib/systemd/user/omacalendard.service" >/dev/null
  # Parse the user units against the staged search path without asking for a
  # live user manager. Release containers intentionally have no login session.
  verify_output="$(SYSTEMD_UNIT_PATH="${staged_root}/usr/lib/systemd/user:" \
    systemd-analyze verify omacalendard.socket omacalendard.service 2>&1 || true)"
  unexpected_output="$(printf '%s\n' "${verify_output}" | \
    sed '\|omacalendard.service: Command /usr/bin/omacalendard is not executable: No such file or directory|d' | \
    sed '/^[[:space:]]*$/d')"
  if [[ -n "${unexpected_output}" ]]; then
    printf '%s\n' "${unexpected_output}" >&2
    exit 1
  fi
fi

echo "staged installation is complete and internally consistent"
