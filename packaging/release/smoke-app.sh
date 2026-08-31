#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 BUILD_DIRECTORY" >&2
  exit 2
fi

build_directory=$(realpath "$1")
daemon="${build_directory}/omacalendard"
control="${build_directory}/omacalendarctl"
application="${build_directory}/src/app/omacalendar"
if [[ ! -x ${application} ]]; then
  application="${build_directory}/omacalendar"
fi
if [[ ! -x ${daemon} || ! -x ${control} || ! -x ${application} ]]; then
  echo "app, daemon, or control binary is missing from ${build_directory}" >&2
  exit 2
fi

smoke_root=$(mktemp -d /tmp/omacalendar-app-smoke.XXXXXX)
daemon_pid=
application_pid=
cleanup() {
  if [[ -n ${application_pid} ]]; then
    kill "${application_pid}" 2>/dev/null || true
    wait "${application_pid}" 2>/dev/null || true
  fi
  if [[ -n ${daemon_pid} ]]; then
    kill "${daemon_pid}" 2>/dev/null || true
    wait "${daemon_pid}" 2>/dev/null || true
  fi
  rm -rf -- "${smoke_root}"
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="${smoke_root}/runtime"
export XDG_DATA_HOME="${smoke_root}/data"
export XDG_CONFIG_HOME="${smoke_root}/config"
export XDG_CACHE_HOME="${smoke_root}/cache"
install -d -m 700 \
  "${XDG_RUNTIME_DIR}" "${XDG_DATA_HOME}" "${XDG_CONFIG_HOME}" "${XDG_CACHE_HOME}"

"${daemon}" >"${smoke_root}/daemon.log" 2>&1 &
daemon_pid=$!
for _attempt in {1..50}; do
  if "${control}" system.health >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "${daemon_pid}" 2>/dev/null; then
    echo "daemon exited before desktop startup" >&2
    sed -n '1,200p' "${smoke_root}/daemon.log" >&2
    exit 1
  fi
  sleep 0.1
done
if ! "${control}" system.health >/dev/null 2>&1; then
  echo "daemon did not become ready for desktop startup" >&2
  sed -n '1,200p' "${smoke_root}/daemon.log" >&2
  exit 1
fi

QT_QPA_PLATFORM=offscreen \
QT_QPA_PLATFORMTHEME='' \
QT_QUICK_CONTROLS_STYLE=Basic \
QT_QUICK_BACKEND=software \
QSG_RHI_BACKEND=software \
QML_DISABLE_DISK_CACHE=1 \
  "${application}" >"${smoke_root}/application.log" 2>&1 &
application_pid=$!

for _attempt in {1..30}; do
  if ! kill -0 "${application_pid}" 2>/dev/null; then
    echo "desktop application exited during startup smoke test" >&2
    sed -n '1,240p' "${smoke_root}/application.log" >&2
    exit 1
  fi
  sleep 0.1
done

if grep -Eiq \
  'QQmlApplicationEngine failed|failed to load component|module .* is not installed|ReferenceError:|TypeError:|Cannot assign to non-existent' \
  "${smoke_root}/application.log"; then
  echo "desktop application reported a QML startup failure" >&2
  sed -n '1,240p' "${smoke_root}/application.log" >&2
  exit 1
fi

echo "desktop application stayed healthy through isolated offscreen startup"
