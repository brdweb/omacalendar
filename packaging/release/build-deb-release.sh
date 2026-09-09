#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 VERSION OUTPUT_DIRECTORY" >&2
  exit 2
fi
repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "${repository_root}/packaging/release/version-lib.sh"
source "${repository_root}/packaging/release/deb-version-lib.sh"
release_version=$1
package_filename=$(deb_package_filename "${release_version}")
if [[ $(release_base_version "${release_version}") != \
      $(release_base_version "$(cmake_release_version "${repository_root}")") ]]; then
  echo "requested package version does not match the CMake project version" >&2
  exit 2
fi
if [[ ! ${SOURCE_DATE_EPOCH:-} =~ ^[1-9][0-9]*$ ]]; then
  echo "SOURCE_DATE_EPOCH must be a positive Unix timestamp" >&2
  exit 2
fi
if [[ $(dpkg --print-architecture) != amd64 ]] || \
  ! grep -Fxq 'VERSION_ID="26.04"' /etc/os-release || \
  ! grep -Fxq 'ID=ubuntu' /etc/os-release; then
  echo "build in the packaging/debian/Dockerfile Ubuntu 26.04 amd64 environment" >&2
  exit 2
fi
mkdir -p "$2"
output_directory=$(realpath "$2")
libical_version=4.0.5
libical_sha256=cc09a3ac41d60e6144e644bd3fcf97d47106d659c4a0b8965102581401e67c9c
libical_archive="libical-${libical_version}.tar.gz"
for output_name in "${package_filename}" \
  "${libical_archive}" "omacalendar-${release_version}-ubuntu26.04-build-packages.txt"; do
  if [[ -e ${output_directory}/${output_name} || -L ${output_directory}/${output_name} ]]; then
    echo "refusing to overwrite existing release output: ${output_name}" >&2
    exit 1
  fi
done
working_directory=$(mktemp -d /tmp/omacalendar-deb-release.XXXXXX)
trap 'rm -rf -- "${working_directory}"' EXIT
parallel_jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-2}
curl --fail --location --retry 3 --silent --show-error \
  "https://github.com/libical/libical/releases/download/v${libical_version}/${libical_archive}" \
  --output "${working_directory}/${libical_archive}"
printf '%s  %s\n' "${libical_sha256}" "${working_directory}/${libical_archive}" | \
  sha256sum --check
tar -xzf "${working_directory}/${libical_archive}" -C "${working_directory}"
libical_source="${working_directory}/libical-${libical_version}"
libical_prefix="${working_directory}/libical-install"
cmake -S "${libical_source}" -B "${working_directory}/libical-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${libical_prefix}" \
  -DCMAKE_INSTALL_LIBDIR=lib -DLIBICAL_CXX_BINDINGS=OFF \
  -DLIBICAL_JAVA_BINDINGS=OFF -DLIBICAL_GLIB=OFF \
  -DLIBICAL_GOBJECT_INTROSPECTION=OFF -DLIBICAL_GLIB_VAPI=OFF \
  -DLIBICAL_BUILD_DOCS=OFF -DLIBICAL_BUILD_EXAMPLES=OFF \
  -DLIBICAL_BUILD_TESTING=OFF -DLIBICAL_ENABLE_BUILTIN_TZDATA=OFF \
  -DCMAKE_C_FLAGS="-ffile-prefix-map=${working_directory}=/usr/src/omacalendar" \
  -DCMAKE_SHARED_LINKER_FLAGS='-Wl,-z,relro,-z,now'
cmake --build "${working_directory}/libical-build" --parallel "${parallel_jobs}"
cmake --install "${working_directory}/libical-build"
export PKG_CONFIG_PATH="${libical_prefix}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
export LD_LIBRARY_PATH="${libical_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
cmake -S "${repository_root}" -B "${working_directory}/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_RPATH=/usr/lib/omacalendar \
  -DCMAKE_CXX_FLAGS="-ffile-prefix-map=${working_directory}=/usr/src/omacalendar -ffile-prefix-map=${repository_root}=/usr/src/omacalendar" \
  -DOMACALENDAR_VERSION_SUFFIX="$(release_version_suffix "${release_version}")" \
  -DOMACALENDAR_REQUIRE_GOOGLE_OAUTH_CONFIG="${OMACALENDAR_REQUIRE_GOOGLE_OAUTH_CONFIG:-OFF}"
cmake --build "${working_directory}/build" --parallel "${parallel_jobs}"
QT_QPA_PLATFORM=offscreen ctest --test-dir "${working_directory}/build" \
  --output-on-failure --parallel "${parallel_jobs}"
stage_root="${working_directory}/stage"
DESTDIR="${stage_root}" cmake --install "${working_directory}/build" --strip
install -d "${stage_root}/usr/lib/omacalendar" \
  "${stage_root}/usr/share/doc/omacalendar/libical"
cp -a "${libical_prefix}/lib/"libical.so.* "${stage_root}/usr/lib/omacalendar/"
cp "${repository_root}/LICENSE" "${stage_root}/usr/share/doc/omacalendar/copyright"
cp "${libical_source}/LICENSE.txt" "${stage_root}/usr/share/doc/omacalendar/libical/"
cp -a "${libical_source}/LICENSES" "${stage_root}/usr/share/doc/omacalendar/libical/"
cp "${repository_root}/packaging/debian/README.md" \
  "${stage_root}/usr/share/doc/omacalendar/README.Debian"
printf 'Unmodified libical %s source: https://github.com/libical/libical/releases/download/v%s/%s\nSHA256: %s\n' \
  "${libical_version}" "${libical_version}" "${libical_archive}" "${libical_sha256}" \
  >"${stage_root}/usr/share/doc/omacalendar/libical/SOURCE"
bash "${repository_root}/packaging/release/verify-install.sh" "${stage_root}"
bash "${repository_root}/packaging/release/build-deb-package.sh" \
  "${release_version}" "${stage_root}" "${output_directory}"
mkdir "${working_directory}/second-package"
bash "${repository_root}/packaging/release/build-deb-package.sh" \
  "${release_version}" "${stage_root}" "${working_directory}/second-package"
cmp "${output_directory}/${package_filename}" \
  "${working_directory}/second-package/${package_filename}"
cp "${working_directory}/${libical_archive}" "${output_directory}/${libical_archive}"
dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' | LC_ALL=C sort \
  >"${output_directory}/omacalendar-${release_version}-ubuntu26.04-build-packages.txt"
echo "Debian release build, automated tests, install verification and package reproducibility passed"
