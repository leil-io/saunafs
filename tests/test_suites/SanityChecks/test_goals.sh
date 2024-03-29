CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"
for goal in 1 2 3; do
	mkdir goal_$goal
	saunafs setgoal $goal goal_$goal
	file=goal_$goal/file
	FILE_SIZE=12345678 BLOCK_SIZE=12345 file-generate $file
	if ! saunafs checkfile $file | egrep "chunks with $goal cop(y|ies): *1$"; then
		test_add_failure "File with goal $goal created with undergoal chunks"
	fi
	if ! file-validate $file; then
		test_add_failure "Corrupted data in a file with goal $goal"
	fi
done
