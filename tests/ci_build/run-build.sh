#!/usr/bin/env bash
set -eux -o pipefail
PROJECT_DIR="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"
WORKSPACE=${WORKSPACE:-"${PROJECT_DIR}"}
die() { echo "Error: $*" >&2; exit 1; }

usage() {
	cat <<-EOT
	Builds saunafs with different configurations

	Usage: run-build.sh [OPTION]

	Options:
	  coverage   Build with parameters for coverage report
	  test       Build for test
	  release    Build with no debug info
	EOT
	exit 1
}

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
declare build_dir
shift
case "${build_type,,}" in
	debug)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=Debug
			-DCMAKE_INSTALL_PREFIX="${WORKSPACE}/install/saunafs/"
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=OFF
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=ON
		)
		build_dir="${WORKSPACE}/build/saunafs-debug"
		;;
	coverage)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=Debug
			-DCMAKE_INSTALL_PREFIX="${WORKSPACE}/install/saunafs/"
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=ON
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=OFF
		)
		build_dir="${WORKSPACE}/build/saunafs-coverage"
		;;
	test)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=RelWithDbInfo
			-DCMAKE_INSTALL_PREFIX="${WORKSPACE}/install/saunafs/"
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=OFF
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=ON
		)
		build_dir="${WORKSPACE}/build/saunafs"
		;;
	release)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=Release
			-DCMAKE_INSTALL_PREFIX=/
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

nice make -C "${build_dir}" -j "$(nproc)" install
