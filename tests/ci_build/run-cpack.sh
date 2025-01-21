#!/usr/bin/env bash
set -eu -o pipefail

PROJECT_DIR="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"
build_dir="${PROJECT_DIR}/build/saunafs-release"

if [ -f "${build_dir}/CPackConfig.cmake" ]; then
	nice cpack -B "${build_dir}" --config "${build_dir}/CPackConfig.cmake" -j "$(nproc)"
else
	warn "No CPack configuration found in ${build_dir}. Skipping packaging."
fi
