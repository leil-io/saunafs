MOUNTS=1 \
CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS="caseinsensitive" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# create folder and sample files
mkdir folder1
touch file1
touch folder1/file2

# test write case insensitive
echo "data" > fIlE1
assert_equals "data" "$(cat file1)"

echo "data" > fOLDER1/FIle2
assert_equals "data" "$(cat folder1/file2)"

# test read case insensitive
assert_equals "data" "$(cat FilE1)"
assert_equals "data" "$(cat fOlDeR1/fILe2)"

# test rename case insensitive same file
touch file3
mv file3 filE1
if ls | grep -q "^filE1$"; then
    echo "File filE1 exists"
else
    test_fail "No file named filE1 found"
fi

# test rename case insensitive new file to existing file
touch file3
assert_equals "3" "$(ls . | wc -l)"
mv file3 file1
assert_equals "2" "$(ls . | wc -l)"

# test remove case insensitive
rm "fiLE1"
assert_equals "1" "$(ls . | wc -l)"

rm "foldeR1/fIlE2"
assert_equals "0" "$(ls foldeR1/fIlE2 | wc -l)"
