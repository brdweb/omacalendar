#!/usr/bin/env bash
# Acceptance helper run inside a disposable installed Flatpak sandbox.
set -euo pipefail
role=$1
profile=$2
child_pid=
cleanup() {
  if [[ -n ${child_pid} ]]; then
    kill "${child_pid}" 2>/dev/null || true
    wait "${child_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'exit 143' TERM HUP
trap 'exit 130' INT
if [[ ${role} == owner ]]; then
  /app/bin/omacalendar --daemon >"${profile}/owner.log" 2>&1 &
else
  QSG_INFO=1 /app/bin/omacalendar >"${profile}/desktop.log" 2>&1 &
fi
child_pid=$!
ready=false
for _attempt in {1..300}; do
  kill -0 "${child_pid}" 2>/dev/null || exit 1
  if [[ ${role} == owner ]]; then
    if [[ -S ${XDG_RUNTIME_DIR}/app/${FLATPAK_ID}/omacalendar/daemon.sock ]] &&
       /app/bin/omacalendarctl system.info >/dev/null 2>&1; then
      ready=true
    fi
  elif grep -q 'qt.scenegraph.general:' "${profile}/desktop.log"; then
    ready=true
  fi
  [[ ${ready} != true ]] || break
  sleep 0.1
done
[[ ${ready} == true ]]
touch "${profile}/${role}-ready"
for _attempt in {1..600}; do
  kill -0 "${child_pid}" 2>/dev/null || exit 1
  if [[ -e ${profile}/stop-${role} ]]; then
    kill -TERM "${child_pid}"
    wait "${child_pid}" || [[ $? == 143 ]]
    child_pid=
    touch "${profile}/${role}-stopped"
    exit 0
  fi
  sleep 0.1
done
exit 1
