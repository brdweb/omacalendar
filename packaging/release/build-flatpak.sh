#!/usr/bin/env bash
set -euo pipefail
umask 077

if [[ $# -lt 3 || $# -gt 4 || ( $# == 4 && $4 != --without-google-config ) ]]; then
  echo "usage: $0 SOURCE_TARBALL RELEASE_VERSION OUTPUT_DIRECTORY [--without-google-config]" >&2
  exit 2
fi
script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=version-lib.sh
source "${script_directory}/version-lib.sh"
source_archive=$(realpath "$1")
release_version=$2
validate_release_version "${release_version}"
[[ -f ${source_archive} ]]
[[ $(uname -m) == x86_64 ]] || { echo 'The release Flatpak targets x86_64.' >&2; exit 1; }
mkdir -p "$3"
output_directory=$(realpath "$3")
for command in flatpak flatpak-builder ostree jq sha256sum curl; do
  command -v "${command}" >/dev/null
done
export OMACALENDAR_FLATPAK_VERSION_SUFFIX
OMACALENDAR_FLATPAK_VERSION_SUFFIX=$(release_version_suffix "${release_version}")
export OMACALENDAR_FLATPAK_REQUIRE_GOOGLE=ON
if [[ ${4:-} == --without-google-config ]]; then
  export OMACALENDAR_FLATPAK_REQUIRE_GOOGLE=OFF
  unset OMACALENDAR_BUILD_GOOGLE_CLIENT_ID OMACALENDAR_BUILD_GOOGLE_CLIENT_SECRET
elif [[ -z ${OMACALENDAR_BUILD_GOOGLE_CLIENT_ID:-} ||
        -z ${OMACALENDAR_BUILD_GOOGLE_CLIENT_SECRET:-} ]]; then
  echo 'Release Flatpaks require the protected Google OAuth build configuration.' >&2
  exit 1
fi

# Keep the build and its generated OAuth header outside the release asset set.
# A caller can retain this directory for diagnosis or repeated builds.
work_directory=${OMACALENDAR_FLATPAK_WORK_DIR:-}
if [[ -z ${work_directory} ]]; then
  work_directory=$(mktemp -d /tmp/omacalendar-flatpak-build.XXXXXX)
else
  mkdir -p "${work_directory}"
  work_directory=$(realpath "${work_directory}")
fi
source_hash=$(sha256sum "${source_archive}" | cut -d' ' -f1)
manifest="${work_directory}/org.omacalendar.OmaCalendar.json"
jq --arg source "${source_archive}" --arg sha256 "${source_hash}" \
  '.modules[-1].sources = [{type: "archive", path: $source, sha256: $sha256}]' \
  "${script_directory}/../flatpak/org.omacalendar.OmaCalendar.json" >"${manifest}"

# FLATPAK_USER_DIR can point at a disposable installation in CI or local testing.
flatpak remote-add --user --if-not-exists flathub \
  https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --assumeyes \
  --force-clean --disable-rofiles-fuse --jobs="${OMACALENDAR_BUILD_JOBS:-2}" \
  --state-dir="${work_directory}/state" --repo="${work_directory}/repo" \
  "${work_directory}/build" "${manifest}"

bundle="${output_directory}/omacalendar-${release_version}-linux-x86_64.flatpak"
flatpak build-bundle --arch=x86_64 \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  "${work_directory}/repo" "${bundle}" org.omacalendar.OmaCalendar stable
[[ ! -e ${output_directory}/flatpak-stage ]] || {
  echo 'The Flatpak SBOM staging destination already exists; choose a fresh output directory.' >&2
  exit 1
}
# Builder's working files can include content exported into separate refs.
# Inventory the exact application ref used by build-bundle, not that superset.
ostree --repo="${work_directory}/repo" checkout --user-mode --force-copy \
  --subpath=/files app/org.omacalendar.OmaCalendar/x86_64/stable \
  "${output_directory}/flatpak-stage"
# Ship the exact LGPL dependency source with the binary. Prefer Builder's
# already verified download, with a checksum-checked fetch for older layouts.
libsecret_url=$(jq -er '.modules[] | select(.name == "libsecret") | .sources[0].url' "${manifest}")
libsecret_hash=$(jq -er '.modules[] | select(.name == "libsecret") | .sources[0].sha256' "${manifest}")
libsecret_filename=${libsecret_url##*/}
libsecret_download="${work_directory}/state/downloads/${libsecret_hash}/${libsecret_filename}"
if [[ -f ${libsecret_download} ]]; then
  cp "${libsecret_download}" "${output_directory}/${libsecret_filename}"
else
  curl --fail --location --silent --show-error "${libsecret_url}" \
    --output "${output_directory}/${libsecret_filename}"
fi
printf '%s  %s\n' "${libsecret_hash}" "${output_directory}/${libsecret_filename}" | \
  sha256sum --check --status
{
  printf 'source_sha256=%s\n' "${source_hash}"
  printf 'runtime=%s\n' 'org.kde.Platform/x86_64/6.10'
  printf 'sdk=%s\n' 'org.kde.Sdk/x86_64/6.10'
  printf 'runtime_commit=%s\n' "$(flatpak info --user --show-commit org.kde.Platform//6.10)"
  printf 'sdk_commit=%s\n' "$(flatpak info --user --show-commit org.kde.Sdk//6.10)"
  printf 'libical=%s\n' '4.0.5'
  printf 'libsecret=%s\n' '0.21.7'
  printf 'google_config_required=%s\n' "${OMACALENDAR_FLATPAK_REQUIRE_GOOGLE}"
} >"${output_directory}/omacalendar-${release_version}-flatpak-runtime.txt"
printf 'Created %s\nBuild directory: %s\n' "${bundle}" "${work_directory}"
