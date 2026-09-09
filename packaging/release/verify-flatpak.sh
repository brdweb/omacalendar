#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 || -z ${FLATPAK_USER_DIR:-} ]]; then
  echo "usage: FLATPAK_USER_DIR=DISPOSABLE_INSTALLATION $0 BUNDLE EXPECTED_VERSION" >&2
  exit 2
fi
bundle=$(realpath "$1")
expected_version=$2
app_id=org.omacalendar.OmaCalendar
for command in flatpak jq timeout flock; do
  command -v "${command}" >/dev/null
done
# Even disposable installations share this app ID's runtime. Serialize the
# entire acceptance run before preflight, keeping this inode outside the app's
# endpoint directory and never unlinking it while another run could hold it.
host_runtime=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
acceptance_lock=${host_runtime}/omacalendar-flatpak-acceptance.lock
if [[ ! -d ${host_runtime} || ! -O ${host_runtime} || -L ${acceptance_lock} ]]; then
  echo 'Refusing acceptance without an owned runtime directory and regular lock path.' >&2
  exit 1
fi
umask 077
exec 8>>"${acceptance_lock}"
if ! flock --exclusive --nonblock 8; then
  echo 'Refusing acceptance while another Flatpak acceptance run is active.' >&2
  exit 1
fi
if flatpak info --user "${app_id}" >/dev/null 2>&1; then
  echo 'Refusing to replace an existing OmaCalendar Flatpak; use a disposable installation.' >&2
  exit 1
fi
if env -u FLATPAK_USER_DIR flatpak info --user "${app_id}" >/dev/null 2>&1 ||
   flatpak info --system "${app_id}" >/dev/null 2>&1; then
  echo 'Refusing acceptance alongside a default user/system OmaCalendar install; use a dedicated test account.' >&2
  exit 1
fi
# Installation directories are separate, but every installation of this app ID
# shares its host runtime endpoint. Refuse real or stale endpoints rather than
# letting a synthetic-profile client attach to an existing user's daemon.
runtime_directory="${host_runtime}/app/${app_id}/omacalendar"
if flatpak ps --columns=application | grep -Fxq "${app_id}"; then
  echo 'Refusing acceptance while any OmaCalendar Flatpak instance is running.' >&2
  exit 1
fi
for endpoint in daemon.sock app-instance.sock; do
  if [[ -e ${runtime_directory}/${endpoint} || -L ${runtime_directory}/${endpoint} ]]; then
    echo "Refusing acceptance with an existing Flatpak endpoint: ${runtime_directory}/${endpoint}" >&2
    exit 1
  fi
done
if [[ -e ${runtime_directory}/clients.lock ]]; then
  exec 9>"${runtime_directory}/clients.lock"
  if ! flock --exclusive --nonblock 9; then
    echo 'Refusing acceptance while another Flatpak launcher owns the runtime.' >&2
    exit 1
  fi
  flock --unlock 9
  exec 9>&-
fi
smoke_root=$(mktemp -d /tmp/omacalendar-flatpak-smoke.XXXXXX)
installed=false
instance_pids=()
cleanup() {
  for instance_pid in "${instance_pids[@]}"; do
    kill "${instance_pid}" 2>/dev/null || true
  done
  for instance_pid in "${instance_pids[@]}"; do
    wait "${instance_pid}" 2>/dev/null || true
  done
  if [[ ${installed} == true ]]; then
    flatpak uninstall --user --noninteractive --assumeyes "${app_id}" >/dev/null || true
  fi
}
trap cleanup EXIT
flatpak install --user --noninteractive --assumeyes "${bundle}"
installed=true
flatpak info --user --show-permissions "${app_id}" >"${smoke_root}/permissions.txt"
if grep -Eq '^sockets=.*(session-bus|system-bus)' \
  "${smoke_root}/permissions.txt"; then
  echo 'The packaged Flatpak unexpectedly requests unfiltered bus access.' >&2
  exit 1
fi
IFS=';' read -r -a filesystem_permissions <<<"$(sed -n 's/^filesystems=//p' "${smoke_root}/permissions.txt")"
for permission in "${filesystem_permissions[@]}"; do
  if [[ -n ${permission} && ${permission} != '!'* ]]; then
    echo "The packaged Flatpak unexpectedly grants host filesystem access: ${permission}" >&2
    exit 1
  fi
done

# Only the disposable test profile is added to the production permissions. Keep
# XDG_RUNTIME_DIR unchanged to exercise the actual per-app sandbox IPC path.
run_options=(--user "--filesystem=${smoke_root}"
  --env=QT_QPA_PLATFORM=offscreen --env=QT_QUICK_BACKEND=software)
# Flatpak sets its own XDG base directories after applying --env. Set the test
# profile within the sandbox so it cannot write the normal Flatpak profile.
profile_environment=("XDG_DATA_HOME=${smoke_root}/data"
  "XDG_CONFIG_HOME=${smoke_root}/config" "XDG_CACHE_HOME=${smoke_root}/cache")
run_app() {
  flatpak run "${run_options[@]}" --command=env "${app_id}" \
    "${profile_environment[@]}" /app/bin/omacalendar "$@"
}

flatpak run "${run_options[@]}" --command=sh "${app_id}" -eu -c '
  test "$FLATPAK_ID" = org.omacalendar.OmaCalendar
  test -x /app/bin/omacalendard
  test -x /app/bin/omacalendarctl
  test -x /app/bin/secret-tool
  test ! -e /app/lib/systemd/user/omacalendard.socket
  test ! -e /app/bin/omacalendar-widgetctl
  test ! -e "$XDG_RUNTIME_DIR/omacalendar/daemon.sock"
  for executable in /app/bin/omacalendard /app/bin/omacalendarctl /app/bin/secret-tool /app/libexec/omacalendar/omacalendar-ui; do
    if ldd "$executable" | grep -q "not found"; then exit 1; fi
  done
'
run_app --cli system.info >"${smoke_root}/system-info.json"
jq -e --arg version "${expected_version}" \
  '.result.version == $version and .result.protocolMajor == 2 and .result.schemaVersion == 2' \
  "${smoke_root}/system-info.json" >/dev/null
run_app --cli calendars.list >"${smoke_root}/calendars.json"
calendar_id=$(jq -er '.result.calendars[] | select(.readOnly == false) | .id' \
  "${smoke_root}/calendars.json" | head -1)
event_params=$(jq -nc --arg calendar "${calendar_id}" \
  '{clientMutationId:"flatpak-acceptance", event:{calendarId:$calendar,summary:"Flatpak restart acceptance",startUtc:"2026-09-08T13:00:00Z",endUtc:"2026-09-08T14:00:00Z",startTimeZone:"UTC",endTimeZone:"UTC"}}')
run_app --cli events.create "${event_params}" >"${smoke_root}/event.json"
event_id=$(jq -er '.result.id' "${smoke_root}/event.json")
run_app --cli settings.set '{"key":"notificationPrivacy","value":"title_only"}' >/dev/null

# Each --cli invocation owns and stops a daemon, so these verify persisted data
# after two complete sandbox exits and subsequent daemon/database reopenings.
run_app --cli events.get "$(jq -nc --arg id "${event_id}" '{eventId:$id}')" \
  >"${smoke_root}/restarted-event.json"
jq -e '.result.summary == "Flatpak restart acceptance"' \
  "${smoke_root}/restarted-event.json" >/dev/null
update_params=$(jq -nc --arg id "${event_id}" \
  --argjson revision "$(jq -er '.result.localRevision' "${smoke_root}/restarted-event.json")" \
  '{eventRef:{eventId:$id},expectedLocalRevision:$revision,clientMutationId:"flatpak-update",recurrenceScope:"series",guestNotificationPolicy:"none",patch:{summary:"Flatpak updated acceptance"}}')
run_app --cli events.update "${update_params}" >"${smoke_root}/updated-event.json"
jq -e '.result.summary == "Flatpak updated acceptance"' \
  "${smoke_root}/updated-event.json" >/dev/null
remove_params=$(jq -nc --arg id "${event_id}" \
  --argjson revision "$(jq -er '.result.localRevision' "${smoke_root}/updated-event.json")" \
  '{eventRef:{eventId:$id},expectedLocalRevision:$revision,clientMutationId:"flatpak-remove",recurrenceScope:"series",guestNotificationPolicy:"none"}')
run_app --cli events.remove "${remove_params}" >"${smoke_root}/removed-event.json"
undo_params=$(jq -ec '{undoToken:.result.undoToken}' "${smoke_root}/removed-event.json")
run_app --cli events.undo "${undo_params}" >"${smoke_root}/restored-event.json"
jq -e '.result.undone == true and .result.event.deleted == false' \
  "${smoke_root}/restored-event.json" >/dev/null
run_app --cli settings.get '{"key":"notificationPrivacy"}' \
  >"${smoke_root}/restarted-setting.json"
grep -q 'title_only' "${smoke_root}/restarted-setting.json"
[[ $(stat -c '%a' "${smoke_root}/data/omacalendar") == 700 ]]
[[ $(stat -c '%a' "${smoke_root}/data/omacalendar/calendar.sqlite3") == 600 ]]

# Two distinct Flatpak instances share the app runtime directory but have
# different PID namespaces. Stop the daemon's owning launcher while a rendered
# desktop is attached, then prove the daemon survives until that desktop exits.
script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cp "${script_directory}/../flatpak/test-instance.sh" "${smoke_root}/test-instance.sh"
start_instance() {
  timeout --signal=TERM --kill-after=5 90s flatpak run "${run_options[@]}" \
    --command=env "${app_id}" "${profile_environment[@]}" \
    bash "${smoke_root}/test-instance.sh" "$1" "${smoke_root}" &
  instance_pids+=("$!")
}
wait_for_marker() {
  for _attempt in {1..400}; do
    [[ ! -e ${smoke_root}/$1 ]] || return 0
    sleep 0.1
  done
  echo "Timed out waiting for Flatpak acceptance marker $1; evidence: ${smoke_root}" >&2
  return 1
}
start_instance owner
wait_for_marker owner-ready
start_instance peer
wait_for_marker peer-ready
# Exercise a peer crash/reopen while the independent daemon owner stays alive.
# Namespace PIDs are often reused, so this catches stale PID-based GUI locks.
touch "${smoke_root}/stop-peer"
wait_for_marker peer-stopped
wait "${instance_pids[1]}"
instance_pids=("${instance_pids[0]}")
mv "${smoke_root}/desktop.log" "${smoke_root}/first-desktop.log"
rm -- "${smoke_root}/peer-ready" "${smoke_root}/peer-stopped" "${smoke_root}/stop-peer"
start_instance peer
wait_for_marker peer-ready
# Perturb the second sandbox's process layout, then require the second desktop
# invocation to activate the existing GUI and exit instead of becoming primary.
timeout --signal=TERM --kill-after=5 15s flatpak run "${run_options[@]}" \
  --command=env "${app_id}" "${profile_environment[@]}" \
  bash -c 'sleep 1 & /app/bin/omacalendar omacalendar://settings/accounts; status=$?; wait; exit "$status"' \
  >"${smoke_root}/second-activation.log" 2>&1
touch "${smoke_root}/stop-owner"
sleep 0.5
[[ ! -e ${smoke_root}/owner-stopped ]]
run_app --cli system.health >"${smoke_root}/concurrent-health.json"
jq -e '.result.ok == true' "${smoke_root}/concurrent-health.json" >/dev/null
touch "${smoke_root}/stop-peer"
wait_for_marker peer-stopped
wait_for_marker owner-stopped
for instance_pid in "${instance_pids[@]}"; do
  wait "${instance_pid}"
done
instance_pids=()
if grep -Eq 'QQmlApplicationEngine failed|Error:|Type .* unavailable' \
  "${smoke_root}/desktop.log" "${smoke_root}/first-desktop.log" "${smoke_root}/second-activation.log"; then
  echo "Flatpak desktop smoke failed; see ${smoke_root}/desktop.log" >&2
  exit 1
fi
run_app --cli system.health >"${smoke_root}/restarted-health.json"
jq -e '.result.ok == true' "${smoke_root}/restarted-health.json" >/dev/null
flatpak uninstall --user --noninteractive --assumeyes "${app_id}" >/dev/null
installed=false
printf 'PASS: Flatpak install, isolated IPC/profile, local CRUD/undo, persisted restart, permissions, rendered desktop, peer reopen, second activation, concurrent daemon lifetime, uninstall\nEvidence: %s\n' "${smoke_root}"
