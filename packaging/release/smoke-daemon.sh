#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 BUILD_DIRECTORY [EXPECTED_VERSION]" >&2
  exit 2
fi

build_directory=$(realpath "$1")
daemon="${build_directory}/omacalendard"
control="${build_directory}/omacalendarctl"
expected_version=${2:-}
if [[ ! -x ${daemon} || ! -x ${control} ]]; then
  echo "daemon or control binary is missing from ${build_directory}" >&2
  exit 2
fi

smoke_root=$(mktemp -d /tmp/omacalendar-smoke.XXXXXX)
daemon_pid=
cleanup() {
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

start_daemon() {
  "${daemon}" >>"${smoke_root}/daemon.log" 2>&1 &
  daemon_pid=$!
}

wait_for_method() {
  local method=$1
  local output=$2
  for _attempt in {1..50}; do
    if "${control}" "${method}" >"${output}" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "${daemon_pid}" 2>/dev/null; then
      echo "daemon exited during smoke test" >&2
      cat "${smoke_root}/daemon.log" >&2
      return 1
    fi
    sleep 0.1
  done
  echo "daemon did not answer ${method}" >&2
  cat "${smoke_root}/daemon.log" >&2
  return 1
}

start_daemon
wait_for_method system.info "${smoke_root}/system-info.json"
grep -Fq '"protocolMajor":2' "${smoke_root}/system-info.json"
grep -Fq '"schemaVersion":2' "${smoke_root}/system-info.json"
grep -Fq '"version":"' "${smoke_root}/system-info.json"
if [[ -n ${expected_version} ]]; then
  grep -Fq "\"version\":\"${expected_version}\"" \
    "${smoke_root}/system-info.json"
fi

[[ $(stat -c '%a' "${XDG_RUNTIME_DIR}/omacalendar") == 700 ]]
[[ $(stat -c '%a' "${XDG_RUNTIME_DIR}/omacalendar/daemon.sock") == 600 ]]
[[ $(stat -c '%a' "${XDG_DATA_HOME}/omacalendar") == 700 ]]
[[ $(stat -c '%a' "${XDG_DATA_HOME}/omacalendar/calendar.sqlite3") == 600 ]]
[[ $(stat -c '%a' "${XDG_CONFIG_HOME}/omacalendar") == 700 ]]
[[ $(stat -c '%a' "${XDG_CACHE_HOME}/omacalendar") == 700 ]]

kill "${daemon_pid}"
wait "${daemon_pid}" 2>/dev/null || true
daemon_pid=

start_daemon
wait_for_method system.health "${smoke_root}/system-health.json"
grep -Fq '"ok":true' "${smoke_root}/system-health.json"

echo "daemon startup, permissions, IPC, and restart smoke test passed"
