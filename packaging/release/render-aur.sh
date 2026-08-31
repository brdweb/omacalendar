#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 VERSION SOURCE_SHA256 BINARY_SHA256 OUTPUT_DIRECTORY" >&2
  exit 2
fi

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=packaging/release/version-lib.sh
source "${repository_root}/packaging/release/version-lib.sh"

release_version=$1
source_sha256=$2
binary_sha256=$3
output_directory=$4

if ! validate_release_version "${release_version}"; then
  echo "release version must be canonical semantic versioning without build metadata" >&2
  exit 2
fi
if [[ ! ${source_sha256} =~ ^[0-9a-f]{64}$ ]]; then
  echo "source checksum must be a lowercase SHA-256 digest" >&2
  exit 2
fi
if [[ ! ${binary_sha256} =~ ^[0-9a-f]{64}$ ]]; then
  echo "binary checksum must be a lowercase SHA-256 digest" >&2
  exit 2
fi

package_version=$(arch_pkgver "${release_version}")
version_suffix=$(release_version_suffix "${release_version}")

mkdir -p "${output_directory}/source" "${output_directory}/binary"

sed \
  -e "s/@ARCH_PKGVER@/${package_version}/g" \
  -e "s/@UPSTREAM_VERSION@/${release_version}/g" \
  -e "s/@VERSION_SUFFIX@/${version_suffix}/g" \
  -e "s/@SOURCE_SHA256@/${source_sha256}/g" \
  "${repository_root}/packaging/aur/PKGBUILD.in" \
  >"${output_directory}/source/PKGBUILD"

sed \
  -e "s/@ARCH_PKGVER@/${package_version}/g" \
  -e "s/@UPSTREAM_VERSION@/${release_version}/g" \
  -e "s/@BINARY_SHA256@/${binary_sha256}/g" \
  "${repository_root}/packaging/aur/PKGBUILD-bin.in" \
  >"${output_directory}/binary/PKGBUILD"
