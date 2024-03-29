#!/usr/bin/env bash
set -eux -o pipefail
PROJECT_DIR="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"
WORKSPACE=${WORKSPACE:-"${PROJECT_DIR}"}
die() { echo "Error: $*" >&2; exit 1; }

export SAUNAFS_ROOT=${WORKSPACE}/install/saunafs
echo "SAUNAFS_ROOT: ${SAUNAFS_ROOT}"
export TEST_OUTPUT_DIR=${WORKSPACE}/test_output
echo "TEST_OUTPUT_DIR: ${TEST_OUTPUT_DIR}"
export TERM=xterm

killall -9 saunafs-tests || true
mkdir -m 777 -p "${TEST_OUTPUT_DIR}"
rm -rf "${TEST_OUTPUT_DIR:?}"/{,.}* || true
rm -rf /mnt/ramdisk/{,.}*  || true
[ -f "${WORKSPACE}/build/saunafs/src/unittests/unittests" ] || \
	die "${WORKSPACE}/build/saunafs/src/unittests/unittests" not found, did you build the project?
"${WORKSPACE}/build/saunafs/src/unittests/unittests" --gtest_color=yes --gtest_output=xml:"${TEST_OUTPUT_DIR}/coverage_test_results.xml"

lcov --directory "${WORKSPACE}/build/saunafs/" --capture --output-file "${TEST_OUTPUT_DIR}/code_coverage.info" -rc lcov_branch_coverage=1
lcov --remove "${TEST_OUTPUT_DIR}/code_coverage.info" -o "${TEST_OUTPUT_DIR}/filtered_code_coverage.info" -rc lcov_branch_coverage=1 \
	'/usr/include/*' \
	'/usr/local/include/*' \
	"${WORKSPACE}/src/unittests/*" \
	"${WORKSPACE}/src/unittests/*" \
	"${WORKSPACE}/src/admin/*_unittest.cc" \
	"${WORKSPACE}/src/chunkserver/*_unittest.cc" \
	"${WORKSPACE}/src/common/*_unittest.cc" \
	"${WORKSPACE}/src/master/*_unittest.cc" \
	"${WORKSPACE}/src/mount/*_unittest.cc" \
	"${WORKSPACE}/src/protocol/*_unittest.cc" \
	"${WORKSPACE}/src/unittests/*"
lcov_cobertura "${TEST_OUTPUT_DIR}/filtered_code_coverage.info" \
	--output "${TEST_OUTPUT_DIR}/coverage.xml" \
	--base-dir "${WORKSPACE}" \
	--demangle
genhtml --output-directory "${TEST_OUTPUT_DIR}/code_coverage_report/" \
	--title "Code coverage report" \
	--branch-coverage --function-coverage --demangle-cpp --legend --num-spaces 2 --sort  \
	"${TEST_OUTPUT_DIR}/filtered_code_coverage.info"
