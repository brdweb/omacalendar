#!/usr/bin/env bash
# Run a development build of omacalendard and omacalendarctl in a private
# sandbox, so a live installation (its database, socket and service) is never
# touched. The daemon gets its own XDG directories and a private D-Bus session,
# so it cannot reach the real keyring or notification daemon.
#
# Usage:
#   scripts/dev-sandbox.sh BUILD_DIR            start the daemon, then open a shell
#   scripts/dev-sandbox.sh BUILD_DIR -- CMD...  start the daemon, run CMD, stop
#
# Inside the sandbox, `omacalendarctl` on PATH is the development build.
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 BUILD_DIR [-- CMD...]" >&2
  exit 2
fi
build_dir=$(cd "$1" && pwd)
shift
if [[ ! -x "${build_dir}/omacalendard" || ! -x "${build_dir}/omacalendarctl" ]]; then
  echo "error: ${build_dir} has no omacalendard/omacalendarctl" >&2
  exit 1
fi
if [[ $# -gt 0 && "$1" == "--" ]]; then
  shift
fi

if [[ -z "${OMACALENDAR_SANDBOX_INNER:-}" ]]; then
  sandbox=$(mktemp -d "${TMPDIR:-/tmp}/omacalendar-dev.XXXXXX")
  export XDG_DATA_HOME="${sandbox}/data" XDG_CONFIG_HOME="${sandbox}/config" \
    XDG_CACHE_HOME="${sandbox}/cache" XDG_STATE_HOME="${sandbox}/state" \
    XDG_RUNTIME_DIR="${sandbox}/runtime"
  install -d -m 700 "${XDG_DATA_HOME}" "${XDG_CONFIG_HOME}" "${XDG_CACHE_HOME}" \
    "${XDG_STATE_HOME}" "${XDG_RUNTIME_DIR}"
  case "${XDG_RUNTIME_DIR}" in
    /run/user/*)
      echo "error: refusing to use the live runtime directory" >&2
      exit 1
      ;;
  esac
  export OMACALENDAR_DISABLE_DAEMON_AUTOSTART=1 OMACALENDAR_SANDBOX_INNER=1
  export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
  unset OMACALENDAR_GOOGLE_CLIENT_ID OMACALENDAR_GOOGLE_CLIENT_SECRET
  echo "sandbox: ${sandbox}" >&2
  exec dbus-run-session -- "$0" "${build_dir}" -- "$@"
fi

"${build_dir}/omacalendard" &
daemon=$!
trap 'kill "${daemon}" 2>/dev/null || true; wait "${daemon}" 2>/dev/null || true' EXIT
export PATH="${build_dir}:${PATH}"
for _ in $(seq 1 50); do
  [[ -S "${XDG_RUNTIME_DIR}/omacalendar/daemon.sock" ]] && break
  sleep 0.1
done

if [[ $# -gt 0 ]]; then
  "$@"
else
  "${SHELL:-bash}" -i
fi
