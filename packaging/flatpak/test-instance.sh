#!/usr/bin/env bash
# Deterministic launcher fixture. It replaces /app binaries with local mocks
# and never requires an installed Flatpak, systemd user service, or GUI stack.
set -euo pipefail
umask 077

script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
launcher=${OMACALENDAR_LAUNCHER_UNDER_TEST:-"${script_directory}/omacalendar-launcher"}
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/omacalendar-launcher.XXXXXX")
runtime_root="${fixture_root}/runtime"
runtime_directory="${runtime_root}/app/org.omacalendar.OmaCalendar/omacalendar"
socket_path="${runtime_directory}/daemon.sock"
trace="${fixture_root}/trace"
gui_ready="${fixture_root}/gui-ready"
gui_release="${fixture_root}/gui-release"
owner_pid=
daemon_mode_pid=

fail() {
  echo "test-instance.sh: $*" >&2
  exit 1
}

cleanup() {
  local status=$?
  trap - EXIT
  for child_pid in "${owner_pid}" "${daemon_mode_pid}"; do
    if [[ -n ${child_pid} ]]; then
      kill "${child_pid}" 2>/dev/null || true
      wait "${child_pid}" 2>/dev/null || true
    fi
  done
  rm -rf -- "${fixture_root}"
  exit "${status}"
}
trap cleanup EXIT

mkdir -p "${fixture_root}/bin"
: >"${trace}"

cat >"${fixture_root}/bin/omacalendard" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exec python3 - "${FIXTURE_SOCKET_PATH:?}" "${FIXTURE_TRACE:?}" <<'PYTHON'
import os
import signal
import socket
import sys

socket_path, trace_path = sys.argv[1:]
os.makedirs(os.path.dirname(socket_path), exist_ok=True)
try:
    os.unlink(socket_path)
except FileNotFoundError:
    pass

server = socket.socket(socket.AF_UNIX)
# The test waits for the listening socket before inspecting this trace, so
# record startup before publishing the readiness state.
with open(trace_path, "a", encoding="utf-8") as trace:
    trace.write("daemon-start\n")
    trace.flush()
server.bind(socket_path)
server.listen()

def stop(_signum, _frame):
    server.close()
    try:
        os.unlink(socket_path)
    except FileNotFoundError:
        pass
    with open(trace_path, "a", encoding="utf-8") as trace:
        trace.write("daemon-stop\n")
    raise SystemExit(0)

signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGHUP, stop)
signal.signal(signal.SIGINT, stop)
while True:
    signal.pause()
PYTHON
EOF

cat >"${fixture_root}/bin/omacalendarctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} == system.ping ]]; then
  [[ -S ${FIXTURE_SOCKET_PATH:?} ]]
  exit
fi
[[ -S ${FIXTURE_SOCKET_PATH:?} ]] || exit 1
printf 'cli-dispatch:%s\n' "${1:-}" >>"${FIXTURE_TRACE:?}"
EOF

cat >"${fixture_root}/bin/omacalendar" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'gui-autostart:%s\n' "${OMACALENDAR_DISABLE_DAEMON_AUTOSTART:-missing}" >>"${FIXTURE_TRACE:?}"
: >"${FIXTURE_GUI_READY:?}"
while [[ ! -e ${FIXTURE_GUI_RELEASE:?} ]]; do
  sleep 0.02
done
printf 'gui-stop\n' >>"${FIXTURE_TRACE:?}"
EOF
chmod +x "${fixture_root}/bin/omacalendard" \
  "${fixture_root}/bin/omacalendarctl" "${fixture_root}/bin/omacalendar"

export FLATPAK_ID=org.omacalendar.OmaCalendar
export XDG_RUNTIME_DIR="${runtime_root}"
export FIXTURE_SOCKET_PATH="${socket_path}"
export FIXTURE_TRACE="${trace}"
export FIXTURE_GUI_READY="${gui_ready}"
export FIXTURE_GUI_RELEASE="${gui_release}"
export OMACALENDAR_LAUNCHER_DAEMON="${fixture_root}/bin/omacalendard"
export OMACALENDAR_LAUNCHER_CLI="${fixture_root}/bin/omacalendarctl"
export OMACALENDAR_LAUNCHER_GUI="${fixture_root}/bin/omacalendar"

wait_until() {
  local description=$1
  shift
  for ((attempt = 0; attempt < 200; ++attempt)); do
    if "$@"; then
      return 0
    fi
    sleep 0.02
  done
  fail "timed out waiting for ${description}"
}

gui_has_started() {
  [[ -e ${gui_ready} ]]
}

daemon_is_listening() {
  [[ -S ${socket_path} ]]
}


# An invalid launch must fail before it invokes any executable. Run this before
# starting the mock daemon so the assertion cannot be satisfied accidentally.
if (
  unset FLATPAK_ID XDG_RUNTIME_DIR
  "${launcher}" --cli system.ping >"${fixture_root}/invalid.stdout" \
    2>"${fixture_root}/invalid.stderr"
); then
  fail 'launcher accepted an invalid Flatpak environment'
fi
[[ ! -s ${trace} ]] || fail 'invalid environment started a fixture executable'

# The first GUI launch owns the first daemon and disables the GUI's detached
# autostart. A concurrent CLI launcher must attach to that daemon instead of
# creating a second one.
"${launcher}" >"${fixture_root}/owner.stdout" 2>"${fixture_root}/owner.stderr" &
owner_pid=$!
wait_until 'first GUI launch' gui_has_started
wait_until 'first daemon socket' daemon_is_listening

[[ $(grep -c '^daemon-start$' "${trace}") == 1 ]] || fail 'first GUI did not own one daemon'
grep -Fx 'gui-autostart:1' "${trace}" >/dev/null || fail 'GUI daemon autostart stayed enabled'

"${launcher}" --cli system.info
[[ $(grep -c '^daemon-start$' "${trace}") == 1 ]] || fail 'concurrent CLI started another daemon'
grep -Fx 'cli-dispatch:system.info' "${trace}" >/dev/null || fail 'CLI mode was not dispatched'
[[ -S ${socket_path} ]] || fail 'attached CLI stopped the owner daemon'

# Releasing the owning GUI lets its launcher take the exclusive clients lock,
# terminate its daemon, and clean the daemon socket.
: >"${gui_release}"
wait "${owner_pid}"
owner_pid=
[[ ! -e ${socket_path} ]] || fail 'owner shutdown left its daemon socket behind'
grep -Fx 'daemon-stop' "${trace}" >/dev/null || fail 'owner shutdown did not stop the daemon'

# --daemon has no GUI client; it owns the daemon until its launcher is stopped.
"${launcher}" --daemon >"${fixture_root}/daemon.stdout" \
  2>"${fixture_root}/daemon.stderr" &
daemon_mode_pid=$!
wait_until 'daemon mode socket' daemon_is_listening
[[ $(grep -c '^daemon-start$' "${trace}") == 2 ]] || fail 'daemon mode did not start its daemon'
kill -TERM "${daemon_mode_pid}"
wait "${daemon_mode_pid}" || [[ $? == 143 ]]
daemon_mode_pid=
[[ ! -e ${socket_path} ]] || fail 'daemon mode shutdown left its socket behind'
[[ $(grep -c '^daemon-stop$' "${trace}") == 2 ]] || fail 'daemon mode did not stop its daemon'
