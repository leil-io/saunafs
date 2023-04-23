CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MASTER_CUSTOM_GOALS="8 X:  _ _ _ trash|14 bettergoal: _ _ _ trash trash" \
	setup_local_empty_saunafs info

# Test set/get goal of a directory for all possible goals
cd "${info[mount0]}"
mkdir directory
for new_goal in {1..7} xor{2..9} X {9..13} bettergoal {15..20} ; do
	assert_equals "directory: $new_goal" "$(saunafs setgoal "$new_goal" directory || echo FAILED)"
	assert_equals "directory: $new_goal" "$(saunafs getgoal directory || echo FAILED)"
done

# Create some files in the directory with different goals...
for goal in 2 3 5 X xor2 xor5 xor7; do
	touch directory/file$goal
	assert_success saunafs setgoal $goal directory/file$goal
	assert_equals "directory/file$goal: $goal" "$(saunafs getgoal directory/file$goal)"
done


# test saunafs setgoal and saunafs getgoal for multiple arguments
assert_success saunafs setgoal 3 directory/file{2..3}
expect_equals $'directory/file2: 3\ndirectory/file3: 3' "$(saunafs getgoal directory/file{2..3})"

# ... and test saunafs setgoal -r with different operations
assert_success saunafs setgoal -r 3 directory
expect_equals "directory/file2: 3" "$(saunafs getgoal directory/file2)"
expect_equals "directory/file3: 3" "$(saunafs getgoal directory/file3)"
expect_equals "directory/file5: 3" "$(saunafs getgoal directory/file5)"
expect_equals "directory/fileX: 3" "$(saunafs getgoal directory/fileX)"
expect_equals "directory/filexor2: 3" "$(saunafs getgoal directory/filexor2)"
expect_equals "directory/filexor5: 3" "$(saunafs getgoal directory/filexor5)"
expect_equals "directory/filexor7: 3" "$(saunafs getgoal directory/filexor7)"

assert_success saunafs setgoal -r 4 directory
expect_equals "directory/file2: 4" "$(saunafs getgoal directory/file2)"
expect_equals "directory/file3: 4" "$(saunafs getgoal directory/file3)"
expect_equals "directory/file5: 4" "$(saunafs getgoal directory/file5)"
expect_equals "directory/fileX: 4" "$(saunafs getgoal directory/fileX)"
expect_equals "directory/filexor2: 4" "$(saunafs getgoal directory/filexor2)"
expect_equals "directory/filexor5: 4" "$(saunafs getgoal directory/filexor5)"
expect_equals "directory/filexor7: 4" "$(saunafs getgoal directory/filexor7)"

assert_success saunafs setgoal -r 3 directory
expect_equals "directory/file2: 3" "$(saunafs getgoal directory/file2)"
expect_equals "directory/file3: 3" "$(saunafs getgoal directory/file3)"
expect_equals "directory/file5: 3" "$(saunafs getgoal directory/file5)"
expect_equals "directory/fileX: 3" "$(saunafs getgoal directory/fileX)"
expect_equals "directory/filexor2: 3" "$(saunafs getgoal directory/filexor2)"
expect_equals "directory/filexor5: 3" "$(saunafs getgoal directory/filexor5)"
expect_equals "directory/filexor7: 3" "$(saunafs getgoal directory/filexor7)"

assert_success saunafs setgoal -r bettergoal directory
expect_equals "directory/file2: bettergoal" "$(saunafs getgoal directory/file2)"
expect_equals "directory/file3: bettergoal" "$(saunafs getgoal directory/file3)"
expect_equals "directory/file5: bettergoal" "$(saunafs getgoal directory/file5)"
expect_equals "directory/fileX: bettergoal" "$(saunafs getgoal directory/fileX)"
expect_equals "directory/filexor2: bettergoal" "$(saunafs getgoal directory/filexor2)"
expect_equals "directory/filexor5: bettergoal" "$(saunafs getgoal directory/filexor5)"
expect_equals "directory/filexor7: bettergoal" "$(saunafs getgoal directory/filexor7)"
