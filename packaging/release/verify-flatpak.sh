#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 || -z ${FLATPAK_USER_DIR:-} ]]; then
  echo "usage: FLATPAK_USER_DIR=DISPOSABLE_INSTALLATION $0 BUNDLE EXPECTED_VERSION" >&2
  exit 2
fi
bundle=$(realpath "$1")
expected_version=$2
app_id=org.omacalendar.OmaCalendar
for command in flatpak jq timeout; do
  command -v "${command}" >/dev/null
done
if flatpak info --user "${app_id}" >/dev/null 2>&1; then
  echo 'Refusing to replace an existing OmaCalendar Flatpak; use a disposable installation.' >&2
  exit 1
fi
smoke_root=$(mktemp -d /tmp/omacalendar-flatpak-smoke.XXXXXX)
installed=false
cleanup() {
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
  "--env=XDG_DATA_HOME=${smoke_root}/data"
  "--env=XDG_CONFIG_HOME=${smoke_root}/config"
  "--env=XDG_CACHE_HOME=${smoke_root}/cache"
  --env=QT_QPA_PLATFORM=offscreen --env=QT_QUICK_BACKEND=software)
run_app() { flatpak run "${run_options[@]}" "${app_id}" "$@"; }

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
jq -e '.result.event.summary == "Flatpak updated acceptance"' \
  "${smoke_root}/updated-event.json" >/dev/null
remove_params=$(jq -nc --arg id "${event_id}" \
  --argjson revision "$(jq -er '.result.event.localRevision' "${smoke_root}/updated-event.json")" \
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

set +e
timeout --signal=TERM --kill-after=5 5s flatpak run "${run_options[@]}" \
  "${app_id}" >"${smoke_root}/desktop.log" 2>&1
desktop_status=$?
set -e
if [[ ${desktop_status} != 124 ]] ||
   grep -Eq 'QQmlApplicationEngine failed|Error:|Type .* unavailable' "${smoke_root}/desktop.log"; then
  echo "Flatpak desktop smoke failed; see ${smoke_root}/desktop.log" >&2
  exit 1
fi
run_app --cli system.health >"${smoke_root}/restarted-health.json"
jq -e '.result.ok == true' "${smoke_root}/restarted-health.json" >/dev/null
flatpak uninstall --user --noninteractive --assumeyes "${app_id}" >/dev/null
installed=false
printf 'PASS: Flatpak install, isolated IPC/profile, local CRUD, persisted restart, permissions, desktop startup, uninstall\nEvidence: %s\n' "${smoke_root}"
