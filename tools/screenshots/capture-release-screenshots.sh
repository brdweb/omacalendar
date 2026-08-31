#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${script_dir}/../.." && pwd)"
output_dir="${repo_dir}/docs/screenshots"
capture_tmp="$(mktemp -d)"
trap 'rm -rf -- "${capture_tmp}"' EXIT

fixture_module="${capture_tmp}/imports/OmaCalendar"
raw_dir="${capture_tmp}/cache/omacalendar-screenshots"
mkdir -p -- "${fixture_module}" "${raw_dir}" "${output_dir}"

install -m 0644 "${script_dir}/fixtures/qmldir" "${fixture_module}/qmldir"
install -m 0644 "${script_dir}/fixtures/App.qml" "${fixture_module}/App.qml"
install -m 0644 "${script_dir}/fixtures/OmarchyTheme.qml" \
  "${fixture_module}/OmarchyTheme.qml"
install -m 0644 "${repo_dir}/src/app/qml/Theme.qml" \
  "${fixture_module}/Theme.qml"
test "$(rg -c 'OmarchyTheme\.onAccent' "${fixture_module}/Theme.qml")" = "1"
sed -i 's/OmarchyTheme\.onAccent/OmarchyTheme.accentForeground/' \
  "${fixture_module}/Theme.qml"

export XDG_CONFIG_HOME="${capture_tmp}/config"
export XDG_DATA_HOME="${capture_tmp}/data"
export XDG_CACHE_HOME="${capture_tmp}/cache"
export XDG_STATE_HOME="${capture_tmp}/state"
export XDG_RUNTIME_DIR="${capture_tmp}/runtime"
export LC_ALL=C.UTF-8
export TZ=America/New_York
export QT_QPA_PLATFORM=offscreen
export QT_QPA_PLATFORMTHEME=
export QT_QUICK_CONTROLS_STYLE=Basic
export QT_QUICK_BACKEND=software
export QSG_RHI_BACKEND=software
export QML_DISABLE_DISK_CACHE=1
export QT_SCALE_FACTOR=1
export QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough
unset DISPLAY WAYLAND_DISPLAY QT_IM_MODULE XDG_CURRENT_DESKTOP

mkdir -m 0700 -p -- "${XDG_RUNTIME_DIR}"

qml_test_runner="${QMLTESTRUNNER:-}"
if [[ -z "${qml_test_runner}" ]]; then
  for candidate in \
    /usr/lib/qt6/bin/qmltestrunner \
    /usr/lib64/qt6/bin/qmltestrunner \
    "$(command -v qmltestrunner6 2>/dev/null || true)"; do
    if [[ -n "${candidate}" && -x "${candidate}" ]]; then
      qml_test_runner="${candidate}"
      break
    fi
  done
fi
if [[ -z "${qml_test_runner}" ]]; then
  echo "Qt 6 qmltestrunner was not found. Set QMLTESTRUNNER to its path." >&2
  exit 1
fi

"${qml_test_runner}" \
  -input "${script_dir}/tst_Capture.qml" \
  -import "${capture_tmp}/imports" \
  -maxwarnings 0

expected=(
  desktop-month.png
  desktop-week.png
  desktop-agenda.png
  event-editor.png
)

for file_name in "${expected[@]}"; do
  raw_file="${raw_dir}/${file_name}"
  final_file="${output_dir}/${file_name}"
  test -s "${raw_file}"
  magick "${raw_file}" \
    -strip \
    -define png:exclude-chunk=date,time \
    -define png:compression-level=9 \
    "PNG32:${final_file}"
  dimensions="$(identify -format '%wx%h' "${final_file}")"
  test "${dimensions}" = "1440x900"
done

ocr_dir="${capture_tmp}/ocr"
mkdir -p -- "${ocr_dir}"
for file_name in "${expected[@]}"; do
  stem="${file_name%.png}"
  tesseract "${output_dir}/${file_name}" "${ocr_dir}/${stem}" 2>/dev/null
done

identity_pattern='/home/'
current_user="$(id -un)"
if [[ "${current_user}" =~ ^[[:alnum:]_.-]+$ ]]; then
  identity_pattern="${identity_pattern}|${current_user}"
fi
ocr_blocked_pattern="(${identity_pattern}|@[[:alnum:]._-]+|gmail|oauth|client[_ -]?secret|access[_ -]?token|refresh[_ -]?token)"
if rg -n -i "${ocr_blocked_pattern}" "${ocr_dir}"; then
  echo "Screenshot privacy check failed: blocked text or embedded data found." >&2
  exit 1
fi

metadata_blocked_pattern="(${identity_pattern}|gmail|oauth|client[_ -]?secret|access[_ -]?token|refresh[_ -]?token)"
for file_name in "${expected[@]}"; do
  final_file="${output_dir}/${file_name}"
  if strings -a "${final_file}" | rg -n -i "${metadata_blocked_pattern}"; then
    echo "Screenshot metadata check failed for ${file_name}." >&2
    exit 1
  fi
  if magick identify -verbose "${final_file}" | rg -q '^  Profiles:'; then
    echo "Screenshot profile check failed for ${file_name}." >&2
    exit 1
  fi
done

printf 'Captured and privacy-checked:\n'
for file_name in "${expected[@]}"; do
  identify -format '  %f  %wx%h  %b\n' "${output_dir}/${file_name}"
done
