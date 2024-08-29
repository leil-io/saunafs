#!/usr/bin/env bash
set -eux -o pipefail
declare -r project_dir="$(realpath "$(dirname "${BASH_SOURCE[0]}")/../..")"

usage() {
	cat <<EOT
Builds saunafs with different configurations

Usage: $0 <build-type> [pack] [extra-cmake-arguments]

Supported build types:
  debug: Debug build with tests and code coverage
  coverage: Debug build with tests and code coverage
  test: RelWithDebInfo build with tests
  release: RelWithDebInfo build without tests

If 'pack' is specified, the build artifacts will be packaged.

Extra CMake arguments can be passed after the build type.

EOT
}

die() { echo "Error: $*" >&2; usage; exit 1; }
warn() { echo "Warning: $*" >&2; }

is_truthy() { grep -Eqi '^(true|yes|on|1|enable[d]?)$' <<<"${1:-}"; }

: "${WORKSPACE:="${project_dir}"}"

declare -a CMAKE_SAUNAFS_ARGUMENTS=(
	-G 'Unix Makefiles'
	-DENABLE_DOCS=ON
	-DENABLE_CLIENT_LIB=ON
	-DENABLE_URAFT=ON
	-DENABLE_NFS_GANESHA=ON
	-DGSH_CAN_HOST_LOCAL_FS=ON
	-DENABLE_POLONAISE=OFF
)

[ -n "${1:-}" ] || usage
declare build_type="${1}"
shift
declare pack=false
if [ -n "${1:-}" ] && [ "${1}" = 'pack' ]; then
	shift
	pack=true
fi

declare build_dir
declare -a make_extra_args=()
case "${build_type,,}" in
	debug)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=Debug
			-DCMAKE_INSTALL_PREFIX="${WORKSPACE}/install/saunafs/"
			-DENABLE_DISTCC=ON
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=OFF
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=ON
		)
		build_dir="${WORKSPACE}/build/saunafs-debug"
		make_extra_args+=( 'install' )
		;;
	coverage)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=Debug
			-DCMAKE_INSTALL_PREFIX="${WORKSPACE}/install/saunafs/"
			-DENABLE_DISTCC=ON
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=ON
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=OFF
		)
		build_dir="${WORKSPACE}/build/saunafs-coverage"
		make_extra_args+=( 'install' )
		;;
	test)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=RelWithDebInfo
			-DCMAKE_INSTALL_PREFIX="${WORKSPACE}/install/saunafs/"
			-DENABLE_DISTCC=ON
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=OFF
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=ON
		)
		build_dir="${WORKSPACE}/build/saunafs"
		make_extra_args+=( 'install' )
		;;
	release)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=RelWithDebInfo
			-DCMAKE_INSTALL_PREFIX=/
			-DENABLE_DISTCC=ON
			-DENABLE_TESTS=OFF
			-DCODE_COVERAGE=OFF
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=OFF
			-DENABLE_WERROR=OFF
		)
		build_dir="${WORKSPACE}/build/saunafs-release"
		;;
	*) die "Unsupported build type: ${build_type}"
		;;
esac

if [ -n "${PACKAGE_VERSION:-}" ]; then
	CMAKE_SAUNAFS_ARGUMENTS+=( -DPACKAGE_VERSION="${PACKAGE_VERSION}" )
fi

declare -a EXTRA_ARGUMENTS=("${@}")
# shellcheck disable=SC2115
rm -r "${build_dir:?}"/{,.}* 2>/dev/null || true
cmake -B "${build_dir}" \
	"${CMAKE_SAUNAFS_ARGUMENTS[@]}" \
	"${EXTRA_ARGUMENTS[@]}" "${WORKSPACE}"

nice make -C "${build_dir}" -j "$(nproc)" "${make_extra_args[@]}"

if ! is_truthy "${pack}"; then
	exit 0
fi
if [ -f "${build_dir}/CPackConfig.cmake" ]; then
	nice cpack -B "${build_dir}" --config "${build_dir}/CPackConfig.cmake" -j "$(nproc)"
else
	warn "No CPack configuration found in ${build_dir}. Skipping packaging."
fi
