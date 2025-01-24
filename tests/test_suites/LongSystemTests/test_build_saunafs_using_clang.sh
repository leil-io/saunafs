timeout_set 20 minutes
assert_program_installed git
assert_program_installed cmake
assert_program_installed make
assert_program_installed clang++-19

MINIMUM_PARALLEL_JOBS=4
MAXIMUM_PARALLEL_JOBS=16
PARALLEL_JOBS=$(get_nproc_clamped_between ${MINIMUM_PARALLEL_JOBS} ${MAXIMUM_PARALLEL_JOBS})

# Change to SOURCE_DIR
cp -r "${SOURCE_DIR}" "${TEMP_DIR}"
cd "${TEMP_DIR}/${SOURCE_DIR}"

BUILD_DIR=$(mktemp -d)
chmod 744 "${BUILD_DIR}"

assert_success cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++-19 \
	-DENABLE_TESTS=ON -DENABLE_NFS_GANESHA=ON -DENABLE_CLIENT_LIB=ON
assert_success cmake --build "${BUILD_DIR}" -j"${PARALLEL_JOBS}"

rm -rf "${BUILD_DIR}"
