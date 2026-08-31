#!/usr/bin/env bash

# Shared semantic-version helpers for release tooling. Build metadata is omitted
# deliberately: immutable release artifacts and tags use one canonical version.

validate_release_version() {
  if [[ $# -ne 1 ]]; then
    return 1
  fi
  local version=$1
  if [[ ! ${version} =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-([0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*))?$ ]]; then
    return 1
  fi

  local prerelease=${BASH_REMATCH[5]}
  if [[ -n ${prerelease} ]]; then
    local identifier
    local -a identifiers
    IFS=. read -r -a identifiers <<<"${prerelease}"
    for identifier in "${identifiers[@]}"; do
      if [[ ${identifier} =~ ^[0-9]+$ && ${#identifier} -gt 1 && ${identifier} == 0* ]]; then
        return 1
      fi
    done
  fi
  return 0
}

release_base_version() {
  local version=$1
  printf '%s\n' "${version%%-*}"
}

release_version_suffix() {
  local version=$1
  local base
  base=$(release_base_version "${version}")
  printf '%s\n' "${version#"${base}"}"
}

arch_pkgver() {
  if [[ $# -ne 1 ]] || ! validate_release_version "$1"; then
    return 1
  fi

  local version=$1
  local base
  base=$(release_base_version "${version}")
  if [[ ${version} == "${base}" ]]; then
    printf '%s\n' "${base}"
    return 0
  fi

  # Keep the SemVer-to-Arch mapping injective by limiting Arch prereleases to
  # the supported release channels and, optionally, one numeric identifier.
  # Arch treats a separator after the patch number as a newer version, so drop
  # the remaining dot (for example, 1.0.0-beta.1 becomes 1.0.0beta1).
  local prerelease=${version#*-}
  if [[ ! ${prerelease} =~ ^(alpha|beta|rc)(\.(0|[1-9][0-9]*))?$ ]]; then
    return 1
  fi
  local arch_prerelease=${prerelease//./}
  printf '%s%s\n' "${base}" "${arch_prerelease}"
}

requires_public_release_gates() {
  if [[ $# -ne 1 ]] || ! validate_release_version "$1"; then
    return 1
  fi
  local version=$1

  # The already-published 1.0.0-alpha record predates the expanded public
  # release checklist. Every later stable or prerelease version must use it.
  [[ ${version} != 1.0.0-alpha ]]
}

cmake_release_version() {
  local repository_root=$1
  local numeric_version
  local version_suffix
  numeric_version=$(sed -n \
    '/^project(OmaCalendar$/,/^)/s/^[[:space:]]*VERSION[[:space:]]\+\([0-9.]\+\)$/\1/p' \
    "${repository_root}/CMakeLists.txt")
  version_suffix=$(sed -n \
    's/^set(OMACALENDAR_VERSION_SUFFIX "\([^"]*\)" CACHE STRING$/\1/p' \
    "${repository_root}/CMakeLists.txt")
  if [[ -z ${numeric_version} ]]; then
    echo "could not read the numeric project version from CMakeLists.txt" >&2
    return 1
  fi
  local full_version="${numeric_version}${version_suffix}"
  if ! validate_release_version "${full_version}"; then
    echo "CMake release version is not canonical semantic versioning: ${full_version}" >&2
    return 1
  fi
  printf '%s\n' "${full_version}"
}
