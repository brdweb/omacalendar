#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 VERSION SOURCE_SHA256 BINARY_SHA256 OUTPUT_DIRECTORY" >&2
  exit 2
fi

release_version=$1
source_sha256=$2
binary_sha256=$3
output_directory=$4

if [[ ! ${release_version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "release version must be MAJOR.MINOR.PATCH" >&2
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

mkdir -p "${output_directory}/source" "${output_directory}/binary"

sed \
  -e "s/@PROJECT_VERSION@/${release_version}/g" \
  -e "s/@SOURCE_SHA256@/${source_sha256}/g" \
  packaging/aur/PKGBUILD.in >"${output_directory}/source/PKGBUILD"

sed \
  -e "s/@PROJECT_VERSION@/${release_version}/g" \
  -e "s/@BINARY_SHA256@/${binary_sha256}/g" \
  packaging/aur/PKGBUILD-bin.in >"${output_directory}/binary/PKGBUILD"
