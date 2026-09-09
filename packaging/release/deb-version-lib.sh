#!/usr/bin/env bash

# Source version-lib.sh before this file.
deb_package_version() {
  if [[ $# -ne 1 ]] || ! validate_release_version "$1"; then
    return 1
  fi
  local version=$1
  local base
  base=$(release_base_version "${version}")
  if [[ ${version} == "${base}" ]]; then
    printf '%s-1\n' "${base}"
  else
    printf '%s~%s-1\n' "${base}" "${version#*-}"
  fi
}

# Asset filenames are not Debian versions. GitHub replaces the tilde used in
# prerelease control metadata, so keep the canonical SemVer spelling here.
# Stable filenames remain unchanged. The fixed prefix/suffix and validated
# version use only alphanumerics, underscores, hyphens, and internal periods.
deb_package_filename() {
  if [[ $# -ne 1 ]] || ! validate_release_version "$1"; then
    return 1
  fi
  printf 'omacalendar_%s-1_amd64.deb\n' "$1"
}
