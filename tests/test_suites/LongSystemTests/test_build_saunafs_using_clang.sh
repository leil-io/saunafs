timeout_set 20 minutes
assert_program_installed git
assert_program_installed cmake
assert_program_installed make
assert_program_installed clang

MINIMUM_PARALLEL_JOBS=4
MAXIMUM_PARALLEL_JOBS=16
PARALLEL_JOBS=$(get_nproc_clamped_between ${MINIMUM_PARALLEL_JOBS} ${MAXIMUM_PARALLEL_JOBS})

# Clone the repository
git clone --branch dev https://github.com/leil-io/saunafs.git "${TEMP_DIR}/saunafs"

# Change to the temporary directory
cd "${TEMP_DIR}/saunafs"

# Check if the feature branch exists and check it out if it does
feature_branch="build-saunafs-using-clang"
if git ls-remote --heads origin "${feature_branch}" | \
	grep -q "${feature_branch}"; then
	git checkout "${feature_branch}"
fi

assert_success cmake -B ./build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
	-DENABLE_TESTS=ON -DENABLE_NFS_GANESHA=ON -DENABLE_CLIENT_LIB=ON
assert_success make -C ./build -j${PARALLEL_JOBS}
