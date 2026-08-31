#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -eq 0 ]]; then
  echo "native Arch package tests must run as an unprivileged user" >&2
  exit 2
fi

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/omacalendar-package-test.XXXXXX")
cleanup() {
  rm -rf -- "${temporary_root}"
}
trap cleanup EXIT

mkdir -p \
  "${temporary_root}/stage/usr/bin" \
  "${temporary_root}/stage/usr/share/doc/OmaCalendar" \
  "${temporary_root}/first" \
  "${temporary_root}/second" \
  "${temporary_root}/symlink-output"
printf '#!/usr/bin/env bash\nprintf "OmaCalendar fixture\\n"\n' \
  >"${temporary_root}/stage/usr/bin/omacalendar"
chmod 0755 "${temporary_root}/stage/usr/bin/omacalendar"
printf 'fixture license\n' \
  >"${temporary_root}/stage/usr/share/doc/OmaCalendar/LICENSE"
ln -s omacalendar "${temporary_root}/stage/usr/bin/omacalendar-fixture-link"

export SOURCE_DATE_EPOCH=1700000000
package_filename=omacalendar-1.0.0beta1-1-x86_64.pkg.tar.zst
dangling_target="${temporary_root}/dangling-package-target"
dangling_package="${temporary_root}/symlink-output/${package_filename}"
symlink_error="${temporary_root}/dangling-symlink-error"
ln -s "${dangling_target}" "${dangling_package}"
if "${repository_root}/packaging/release/build-arch-package.sh" \
  1.0.0-beta.1 "${temporary_root}/stage" \
  "${temporary_root}/symlink-output" > /dev/null 2>"${symlink_error}"; then
  echo "native package assembly accepted a dangling output symlink" >&2
  exit 1
fi
if [[ -e ${dangling_target} ]]; then
  echo "native package assembly wrote through a dangling output symlink" >&2
  exit 1
fi
if ! grep -Fq 'refusing to overwrite existing native package:' \
  "${symlink_error}"; then
  echo "native package assembly did not reject the dangling output symlink early" >&2
  exit 1
fi

"${repository_root}/packaging/release/build-arch-package.sh" \
  1.0.0-beta.1 "${temporary_root}/stage" "${temporary_root}/first" >/dev/null
"${repository_root}/packaging/release/build-arch-package.sh" \
  1.0.0-beta.1 "${temporary_root}/stage" "${temporary_root}/second" >/dev/null

first_package="${temporary_root}/first/${package_filename}"
second_package="${temporary_root}/second/${package_filename}"
if [[ ! -f ${first_package} || ! -f ${second_package} ]]; then
  echo "prerelease native package filename is incorrect" >&2
  exit 1
fi
if [[ $(sha256sum "${first_package}" | cut -d' ' -f1) != \
  $(sha256sum "${second_package}" | cut -d' ' -f1) ]]; then
  echo "native package assembly is not reproducible" >&2
  exit 1
fi

workflow="${repository_root}/.github/workflows/release.yml"
arch_output_reference="\${{ steps.version.outputs.arch_pkgver }}"
artifact_reference="artifacts/omacalendar-${arch_output_reference}-1-x86_64.pkg.tar.zst"
for required_wiring in \
  'packaging/release/build-arch-package.sh' \
  'packaging/release/verify-arch-package.sh' \
  "${artifact_reference}"; do
  if ! grep -Fq "${required_wiring}" "${workflow}"; then
    echo "release workflow is missing native package wiring: ${required_wiring}" >&2
    exit 1
  fi
done
if [[ $(grep -Fc "${artifact_reference}" "${workflow}") -lt 2 ]]; then
  echo "native package must carry both provenance and SBOM attestations" >&2
  exit 1
fi

echo "native Arch package reproducibility and release wiring passed"
