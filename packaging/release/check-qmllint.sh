#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 BUILD_DIRECTORY" >&2
  exit 2
fi

build_directory=$1
set +e
lint_output=$(cmake --build "${build_directory}" --target omacalendar_qmllint 2>&1)
lint_status=$?
set -e

printf '%s\n' "${lint_output}"
if (( lint_status != 0 )); then
  exit "${lint_status}"
fi
if grep -Eq '^Warning:' <<<"${lint_output}"; then
  echo "qmllint emitted one or more warnings" >&2
  exit 1
fi
