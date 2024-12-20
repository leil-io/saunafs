assert_program_installed a2x
timeout_set 2 minutes

# Clone the 'dev' branch from the SaunaFS repository
git clone --branch dev https://github.com/leil-io/saunafs.git "${TEMP_DIR}/saunafs"

# Change to the temporary directory
cd "${TEMP_DIR}/saunafs"

# Check if the feature branch exists and check it out if it does
if git ls-remote --heads origin fix-warnings-build-doc | \
	grep -q "fix-warnings-build-doc"; then
	git checkout fix-warnings-build-doc
fi

# Run the build process and capture the output
cmake -B ./build
make -C ./build/doc 2>&1 | tee "${TEMP_DIR}/build-doc.log"

# Check for the presence of 'SyntaxWarning: invalid escape sequence' lines
pattern="SyntaxWarning: invalid escape sequence"
if grep -q "${pattern}" "${TEMP_DIR}/build-doc.log"; then
	echo "Test failed: 'SyntaxWarning: invalid escape sequence' found in a2x output"
	exit 1
else
	echo "Test passed: No 'SyntaxWarning: invalid escape sequence' found in a2x output"
fi
